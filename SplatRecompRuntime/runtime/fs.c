// Wii U file system calls (coreinit FS*, nn_save SAVE*) on top of PC files.
#include "fs.h"
#include "hostfs.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// FSStatus values the game checks for.
#define FS_OK              0
#define FS_END            (-2)
#define FS_MAX            (-3)
#define FS_EXISTS         (-5)
#define FS_NOT_FOUND      (-6)
#define FS_NOT_FILE       (-7)
#define FS_NOT_DIR        (-8)
#define FS_PERMISSION     (-10)

#define FS_STAT_DIRECTORY 0x80000000u
#define FS_STAT_FILE      0x01000000u

// ---------------------------------------------------------------------------
// Mounts
// ---------------------------------------------------------------------------
#define MAX_ROOTS 20   // (16 mods + the game's layers)
typedef struct {
    const char* prefix;           // guest path, e.g. "/vol/content"
    char roots[MAX_ROOTS][1024];  // PC folders, searched in order
    int root_count;
    bool writable;
} Mount;

static Mount g_mounts[8];
static int g_mount_count = 0;
static char g_saves_dir[1024];
static char g_cwd[512] = "/vol/content";
static int g_log_budget = 300;   // how many file opens to print

static Mount* add_mount(const char* prefix, bool writable) {
    Mount* m = &g_mounts[g_mount_count++];
    memset(m, 0, sizeof(*m));
    m->prefix = prefix;
    m->writable = writable;
    return m;
}
static void add_root(Mount* m, const char* dir) {
    if (m->root_count < MAX_ROOTS && host_stat(dir).is_dir)
        snprintf(m->roots[m->root_count++], sizeof(m->roots[0]), "%s", dir);
}

// --- Finding the game folders ------------------------------------------------
typedef struct {
    char dir[1024];
    unsigned long long title_id;
    unsigned version;
} Title;
static Title g_titles[64];
static int g_title_count = 0;

static unsigned long long xml_hex(const char* xml, const char* tag) {
    char open[64];
    snprintf(open, sizeof(open), "<%s", tag);
    const char* p = strstr(xml, open);
    if (!p || !(p = strchr(p, '>'))) return 0;
    return strtoull(p + 1, NULL, 16);
}
static unsigned xml_dec(const char* xml, const char* tag) {
    char open[64];
    snprintf(open, sizeof(open), "<%s", tag);
    const char* p = strstr(xml, open);
    if (!p || !(p = strchr(p, '>'))) return 0;
    return (unsigned)strtoul(p + 1, NULL, 10);
}

static void scan(const char* dir, int depth);
typedef struct { const char* parent; int depth; } ScanCtx;
static void scan_entry(const char* name, bool is_dir, void* user) {
    ScanCtx* ctx = (ScanCtx*)user;
    if (!is_dir) return;
    char sub[1024];
    snprintf(sub, sizeof(sub), "%s/%s", ctx->parent, name);
    scan(sub, ctx->depth + 1);
}
// A title folder has code/content/meta. Its title ID and version are in
// meta/meta.xml, and also in code/app.xml (some dumps lack the meta folder).
static void scan(const char* dir, int depth) {
    static const char* id_files[] = { "meta/meta.xml", "code/app.xml" };
    for (int k = 0; k < 2; k++) {
        char path[1100];
        snprintf(path, sizeof(path), "%s/%s", dir, id_files[k]);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        static char xml[65536];
        size_t n = fread(xml, 1, sizeof(xml) - 1, f);
        fclose(f);
        xml[n] = 0;
        if (g_title_count < 64) {
            Title* t = &g_titles[g_title_count++];
            snprintf(t->dir, sizeof(t->dir), "%s", dir);
            t->title_id = xml_hex(xml, "title_id");
            t->version = xml_dec(xml, "title_version");
            printf("[fs] found title %016llX v%u in %s\n", t->title_id, t->version, dir);
        }
        return;   // a title folder; don't look inside it
    }
    if (depth >= 5) return;
    ScanCtx ctx = { dir, depth };
    host_list_dir(dir, scan_entry, &ctx);
}

bool fs_setup(const char* game_dir, const char* saves_dir, const char* mlc_dir) {
    snprintf(g_saves_dir, sizeof(g_saves_dir), "%s", saves_dir);
    host_mkdirs(g_saves_dir);

    Mount* content = add_mount("/vol/content", false);
    Mount* code = add_mount("/vol/code", false);
    Mount* meta = add_mount("/vol/meta", false);
    Mount* save = add_mount("/vol/save", true);
    add_root(save, g_saves_dir);
    if (mlc_dir && *mlc_dir) add_root(add_mount("/vol/storage_mlc01", false), mlc_dir);

    if (!game_dir || !*game_dir) {
        printf("[fs] no game folder given (--game), so the game can't read its files\n");
        return false;
    }
    scan(game_dir, 0);

    // Pick the game: the update (0005000E...) if there is one, otherwise the
    // base game (00050000...). Some dumps report an update under the base
    // game's ID, so in the end every copy of the same game is layered by
    // version, newest on top. DLC (0005000C...) isn't layered.
    unsigned long long game_low = 0;
    bool have_game = false;
    for (int i = 0; i < g_title_count && !have_game; i++)
        if ((g_titles[i].title_id >> 32) == 0x0005000Eull) { game_low = (u32)g_titles[i].title_id; have_game = true; }
    for (int i = 0; i < g_title_count && !have_game; i++)
        if ((g_titles[i].title_id >> 32) == 0x00050000ull) { game_low = (u32)g_titles[i].title_id; have_game = true; }

    const Title* layers[4];
    int layer_count = 0;
    for (int i = 0; i < g_title_count; i++) {
        const Title* t = &g_titles[i];
        u32 hi = (u32)(t->title_id >> 32);
        if (!have_game || (u32)t->title_id != (u32)game_low) continue;
        if (hi != 0x00050000u && hi != 0x0005000Eu) continue;
        if (layer_count < 4) layers[layer_count++] = t;
    }
    for (int i = 0; i < layer_count; i++)          // newest version first
        for (int j = i + 1; j < layer_count; j++)
            if (layers[j]->version > layers[i]->version) { const Title* x = layers[i]; layers[i] = layers[j]; layers[j] = x; }

    static Title loose;
    if (!layer_count && host_stat(game_dir).is_dir) {
        // No title info found: maybe the folder itself has code/content.
        snprintf(loose.dir, sizeof(loose.dir), "%s", game_dir);
        layers[layer_count++] = &loose;
    }
    {   // mods from the launcher's Mods page: SPLAT_MOD_DIRS="<mod>\content;..." -- their files are found
        // before the game's own (the first listed wins); content folders only (Cemu graphic packs' content\)
        const char* e = getenv("SPLAT_MOD_DIRS");
        if (e && *e) {
            char list[8192];
            snprintf(list, sizeof(list), "%s", e);
            char* save_ptr = NULL;
            for (char* tok = strtok_s(list, ";", &save_ptr); tok; tok = strtok_s(NULL, ";", &save_ptr)) {
                if (content->root_count >= MAX_ROOTS - 4) { printf("[fs] mod skipped (too many): %s\n", tok); continue; }
                int before = content->root_count;
                add_root(content, tok);
                printf("[fs] mod: %s%s\n", tok, content->root_count > before ? "" : " (not a folder: skipped)");
            }
        }
    }
    for (int i = 0; i < layer_count; i++) {
        char p[1100];
        snprintf(p, sizeof(p), "%s/content", layers[i]->dir); add_root(content, p);
        snprintf(p, sizeof(p), "%s/code", layers[i]->dir);    add_root(code, p);
        snprintf(p, sizeof(p), "%s/meta", layers[i]->dir);    add_root(meta, p);
        printf("[fs] layer %d (%s): %s (title %016llX, version %u)\n", i + 1,
               i == 0 && layer_count > 1 ? "update, checked first" : i == layer_count - 1 ? "base game" : "older update",
               layers[i]->dir, layers[i]->title_id, layers[i]->version);
    }
    const Title* base = layer_count ? layers[layer_count - 1] : NULL;
    if (layer_count == 1) printf("[fs] note: only one copy of the game found (no separate update)\n");
    if (!base) printf("[fs] warning: didn't find the game under %s\n", game_dir);
    printf("[fs] saves: %s\n", g_saves_dir);
    return content->root_count > 0;
}

// --- Guest paths -> PC paths --------------------------------------------------
static void normalize(const char* in, char* out, size_t out_size) {
    char tmp[1024];
    if (in[0] == '/') snprintf(tmp, sizeof(tmp), "%s", in);
    else snprintf(tmp, sizeof(tmp), "%s/%s", g_cwd, in);
    // Collapse "//", "/./" and "/x/../".
    char* parts[64];
    int n = 0;
    for (char* tok = strtok(tmp, "/"); tok && n < 64; tok = strtok(NULL, "/")) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (n) n--; continue; }
        parts[n++] = tok;
    }
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len + 2 < out_size; i++)
        len += snprintf(out + len, out_size - len, "/%s", parts[i]);
    if (!n) snprintf(out, out_size, "/");
}

static bool prefix_match(const char* path, const char* prefix, const char** rest) {
    size_t n = strlen(prefix);
    if (strncmp(path, prefix, n) != 0) return false;
    if (path[n] != 0 && path[n] != '/') return false;
    *rest = path + n;
    return true;
}

// Finds the PC path for a guest path. If it exists in any layer, that one;
// otherwise (for creating files) the first layer of a writable mount.
// Returns NULL if the path isn't under any mount.
static const Mount* resolve(const char* guest, char* host, size_t host_size, bool* exists) {
    char path[1024];
    normalize(guest, path, sizeof(path));
    *exists = false;
    for (int i = 0; i < g_mount_count; i++) {
        const Mount* m = &g_mounts[i];
        const char* rest;
        if (!prefix_match(path, m->prefix, &rest) || !m->root_count) continue;
        for (int r = 0; r < m->root_count; r++) {
            snprintf(host, host_size, "%s%s", m->roots[r], rest);
            if (host_stat(host).exists) { *exists = true; return m; }
        }
        snprintf(host, host_size, "%s%s", m->roots[0], rest);
        return m;
    }
    return NULL;
}

static void guest_string(u32 addr, char* out, size_t size) { wiiu_read_string(addr, out, size); }

// --- FSStat --------------------------------------------------------------------
static void write_stat(u32 out, const HostStat* st) {
    for (u32 i = 0; i < 0x64; i += 4) wiiu_write32(out + i, 0);
    wiiu_write32(out + 0x00, st->is_dir ? FS_STAT_DIRECTORY : FS_STAT_FILE);
    wiiu_write32(out + 0x04, 0x666);
    wiiu_write32(out + 0x10, st->is_dir ? 0 : (u32)st->size);
    wiiu_write32(out + 0x14, st->is_dir ? 0 : (u32)st->size);
    // Times: microseconds since 2000-01-01.
    long long t = ((long long)st->mtime - 946684800LL) * 1000000LL;
    if (t < 0) t = 0;
    wiiu_write32(out + 0x24, (u32)(t >> 32)); wiiu_write32(out + 0x28, (u32)t);
    wiiu_write32(out + 0x2C, (u32)(t >> 32)); wiiu_write32(out + 0x30, (u32)t);
}

// ---------------------------------------------------------------------------
// Open files and folders
// ---------------------------------------------------------------------------
#define MAX_FILES 256
#define HANDLE_BASE 0x100u
typedef struct { FILE* f; char path[256]; } OpenFile;
static OpenFile g_files[MAX_FILES];

typedef struct { char name[256]; bool is_dir; } DirName;
typedef struct {
    bool used;
    char host[MAX_ROOTS][1024];
    int host_count;
    DirName* names;
    int count, cap, next;
} OpenDir;
static OpenDir g_dirs[64];

static OpenFile* file_of(u32 handle) {
    u32 i = handle - HANDLE_BASE;
    return (i < MAX_FILES && g_files[i].f) ? &g_files[i] : NULL;
}

static s32 open_file(const char* guest, const char* mode, u32 handle_out) {
    char host[1100];
    bool exists;
    const Mount* m = resolve(guest, host, sizeof(host), &exists);
    bool writing = strpbrk(mode, "wa+") != NULL;
    s32 status = FS_OK;
    FILE* f = NULL;
    if (!m) status = FS_NOT_FOUND;
    else if (writing && !m->writable) status = FS_PERMISSION;
    else if (!exists && !writing) status = FS_NOT_FOUND;
    else if (exists && host_stat(host).is_dir) status = FS_NOT_FILE;
    else {
        char pc_mode[8];
        size_t k = 0;
        for (const char* p = mode; *p && k < 5; p++) if (*p != 'b') pc_mode[k++] = *p;
        pc_mode[k++] = 'b';
        pc_mode[k] = 0;
        // "w" and "a" create the file: make sure its folder exists.
        if (!exists) {
            char dir[1100];
            snprintf(dir, sizeof(dir), "%s", host);
            char* slash = strrchr(dir, '/');
            if (slash) { *slash = 0; host_mkdirs(dir); }
        }
        f = fopen(host, pc_mode);
        if (!f) status = exists ? FS_PERMISSION : FS_NOT_FOUND;
    }
    if (g_log_budget > 0 || status != FS_OK) {
        if (g_log_budget > 0) g_log_budget--;
        static int miss_budget = 200;
        if (status == FS_OK || miss_budget-- > 0)
            printf("[fs] open %s (%s) -> %s\n", guest, mode,
                   status == FS_OK ? "ok" : status == FS_NOT_FOUND ? "not found" : "error");
    }
    if (status != FS_OK) return status;
    if (strstr(guest, "/Model/Fld_")) {   // SPLAT_DETERMINISTIC=1: a scene's map is loading
        extern int splat_deterministic(void);
        extern void splat_det_scene_load(const char* what);
        extern void splat_scene_seen(const char* path);
        if (splat_deterministic()) splat_det_scene_load(guest);
        splat_scene_seen(guest);   // (where the player is: scene_place.c)
    }
    for (u32 i = 0; i < MAX_FILES; i++) {
        if (g_files[i].f) continue;
        g_files[i].f = f;
        snprintf(g_files[i].path, sizeof(g_files[i].path), "%s", guest);
        if (handle_out) wiiu_write32(handle_out, HANDLE_BASE + i);
        return FS_OK;
    }
    fclose(f);
    return FS_MAX;
}

static void add_name(const char* name, bool is_dir, void* user) {
    OpenDir* d = (OpenDir*)user;
    for (int i = 0; i < d->count; i++)
        if (!strcmp(d->names[i].name, name)) return;   // an upper layer has it
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 64;
        d->names = (DirName*)realloc(d->names, d->cap * sizeof(DirName));
    }
    snprintf(d->names[d->count].name, sizeof(d->names[0].name), "%s", name);
    d->names[d->count].is_dir = is_dir;
    d->count++;
}

static s32 open_dir(const char* guest, u32 handle_out) {
    char path[1024];
    normalize(guest, path, sizeof(path));
    for (u32 slot = 0; slot < 64; slot++) {
        if (g_dirs[slot].used) continue;
        OpenDir* d = &g_dirs[slot];
        memset(d, 0, sizeof(*d));
        for (int i = 0; i < g_mount_count; i++) {
            const Mount* m = &g_mounts[i];
            const char* rest;
            if (!prefix_match(path, m->prefix, &rest)) continue;
            for (int r = 0; r < m->root_count; r++) {
                char host[1100];
                snprintf(host, sizeof(host), "%s%s", m->roots[r], rest);
                if (!host_stat(host).is_dir) continue;
                snprintf(d->host[d->host_count++], sizeof(d->host[0]), "%s", host);
                host_list_dir(host, add_name, d);
            }
            break;
        }
        if (!d->host_count) {
            free(d->names);
            if (g_log_budget > 0) printf("[fs] open folder %s -> not found\n", guest);
            return FS_NOT_FOUND;
        }
        d->used = true;
        if (handle_out) wiiu_write32(handle_out, HANDLE_BASE + slot);
        return FS_OK;
    }
    return FS_MAX;
}

static s32 read_dir(u32 handle, u32 entry_out) {
    u32 slot = handle - HANDLE_BASE;
    if (slot >= 64 || !g_dirs[slot].used) return FS_NOT_FOUND;
    OpenDir* d = &g_dirs[slot];
    if (d->next >= d->count) return FS_END;
    DirName* n = &d->names[d->next++];
    HostStat st = { 0 };
    for (int i = 0; i < d->host_count && !st.exists; i++) {
        char host[1400];
        snprintf(host, sizeof(host), "%s/%s", d->host[i], n->name);
        st = host_stat(host);
    }
    write_stat(entry_out, &st);
    u8* name = wiiu_mem_ptr(entry_out + 0x64, 256);
    if (name) snprintf((char*)name, 256, "%s", n->name);
    return FS_OK;
}

static s32 close_dir(u32 handle) {
    u32 slot = handle - HANDLE_BASE;
    if (slot >= 64 || !g_dirs[slot].used) return FS_NOT_FOUND;
    free(g_dirs[slot].names);
    memset(&g_dirs[slot], 0, sizeof(g_dirs[slot]));
    return FS_OK;
}

static s32 stat_path(const char* guest, u32 out) {
    char host[1100];
    bool exists;
    if (strstr(guest, "PostGame_StageView_Fld_")) {   // (looked up on every scene entry: gx2.c's 30 fps per scene)
        extern void splat_scene_seen(const char* path);
        splat_scene_seen(guest);
    }
    resolve(guest, host, sizeof(host), &exists);
    if (!exists) {
        if (g_log_budget > 0) printf("[fs] stat %s -> not found\n", guest);
        return FS_NOT_FOUND;
    }
    HostStat st = host_stat(host);
    write_stat(out, &st);
    return FS_OK;
}

static s32 make_dir(const char* guest) {
    char host[1100];
    bool exists;
    const Mount* m = resolve(guest, host, sizeof(host), &exists);
    if (!m || !m->writable) return FS_PERMISSION;
    if (exists) return FS_EXISTS;
    return host_mkdirs(host) ? FS_OK : FS_PERMISSION;
}

static s32 remove_path(const char* guest) {
    char host[1100];
    bool exists;
    const Mount* m = resolve(guest, host, sizeof(host), &exists);
    if (!m || !m->writable) return FS_PERMISSION;
    if (!exists) return FS_NOT_FOUND;
    return host_remove(host) ? FS_OK : FS_PERMISSION;
}

// ---------------------------------------------------------------------------
// coreinit FS* calls. Most take (client, block, ..., errorMask).
// ---------------------------------------------------------------------------
static void fs_ok(CPUState* c) { ret(c, FS_OK); }
static void FSGetVolumeState(CPUState* c) { ret(c, 1); }   // ready

static void FSOpenFile(CPUState* c) {    // (client, block, path, mode, *handle, mask)
    char path[512], mode[16];
    guest_string(arg(c, 2), path, sizeof(path));
    guest_string(arg(c, 3), mode, sizeof(mode));
    ret(c, (u32)open_file(path, mode, arg(c, 4)));
}
static void FSCloseFile(CPUState* c) {   // (client, block, handle, mask)
    OpenFile* f = file_of(arg(c, 2));
    if (!f) { ret(c, (u32)FS_NOT_FOUND); return; }
    fclose(f->f);
    f->f = NULL;
    ret(c, FS_OK);
}
static u32 do_read(OpenFile* f, u32 buf, u32 size, u32 count) {
    u32 bytes = size * count;
    if (!size || !count) return 0;
    u8* dst = wiiu_mem_ptr(buf, bytes);
    if (!dst) return 0;
    long pos = ftell(f->f);
    size_t got = fread(dst, 1, bytes, f->f);
    wiiu_mem_mark(buf, (u32)got);
    if (strstr(f->path, "Pack/Static.pack")) { extern void splat_bench_pack_read(u32 addr, u32 off, u32 bytes); splat_bench_pack_read(buf, (u32)pos, (u32)got); }   // (the benchmark patches the map list there)
    return (u32)(got / size);
}
static void FSReadFile(CPUState* c) {    // (client, block, buf, size, count, handle, flag, mask)
    OpenFile* f = file_of(arg(c, 5));
    if (!f) { ret(c, (u32)FS_NOT_FOUND); return; }
    ret(c, do_read(f, arg(c, 2), arg(c, 3), arg(c, 4)));
}
static void FSReadFileWithPos(CPUState* c) {  // (client, block, buf, size, count, pos, handle, flag, mask)
    OpenFile* f = file_of(arg(c, 6));
    if (!f) { ret(c, (u32)FS_NOT_FOUND); return; }
    fseek(f->f, (long)arg(c, 5), SEEK_SET);
    ret(c, do_read(f, arg(c, 2), arg(c, 3), arg(c, 4)));
}
static void FSWriteFile(CPUState* c) {   // (client, block, buf, size, count, handle, flag, mask)
    OpenFile* f = file_of(arg(c, 5));
    u32 size = arg(c, 3), count = arg(c, 4);
    if (!f) { ret(c, (u32)FS_NOT_FOUND); return; }
    u32 bytes = size * count;
    u8* src = wiiu_mem_ptr(arg(c, 2), bytes ? bytes : 1);
    if (!src || !size) { ret(c, 0); return; }
    ret(c, (u32)(fwrite(src, 1, (size_t)size * count, f->f) / size));
}
static void FSSetPosFile(CPUState* c) {  // (client, block, handle, pos, mask)
    OpenFile* f = file_of(arg(c, 2));
    if (!f) { ret(c, (u32)FS_NOT_FOUND); return; }
    fseek(f->f, (long)arg(c, 3), SEEK_SET);
    ret(c, FS_OK);
}
static void FSGetPosFile(CPUState* c) {  // (client, block, handle, *pos, mask)
    OpenFile* f = file_of(arg(c, 2));
    if (!f) { ret(c, (u32)FS_NOT_FOUND); return; }
    wiiu_write32(arg(c, 3), (u32)ftell(f->f));
    ret(c, FS_OK);
}
static void FSIsEof(CPUState* c) {       // (client, block, handle, mask)
    OpenFile* f = file_of(arg(c, 2));
    if (!f) { ret(c, (u32)FS_NOT_FOUND); return; }
    long pos = ftell(f->f);
    fseek(f->f, 0, SEEK_END);
    long end = ftell(f->f);
    fseek(f->f, pos, SEEK_SET);
    ret(c, pos >= end ? (u32)FS_END : FS_OK);
}
static void FSGetStatFile(CPUState* c) { // (client, block, handle, *stat, mask)
    OpenFile* f = file_of(arg(c, 2));
    if (!f) { ret(c, (u32)FS_NOT_FOUND); return; }
    long pos = ftell(f->f);
    fseek(f->f, 0, SEEK_END);
    HostStat st = { true, false, (uint64_t)ftell(f->f), 0 };
    fseek(f->f, pos, SEEK_SET);
    write_stat(arg(c, 3), &st);
    ret(c, FS_OK);
}
static void FSFlushFile(CPUState* c) {
    OpenFile* f = file_of(arg(c, 2));
    if (f) fflush(f->f);
    ret(c, f ? FS_OK : (u32)FS_NOT_FOUND);
}
static void FSGetStat(CPUState* c) {     // (client, block, path, *stat, mask)
    char path[512];
    guest_string(arg(c, 2), path, sizeof(path));
    ret(c, (u32)stat_path(path, arg(c, 3)));
}
static void FSOpenDir(CPUState* c) {     // (client, block, path, *handle, mask)
    char path[512];
    guest_string(arg(c, 2), path, sizeof(path));
    ret(c, (u32)open_dir(path, arg(c, 3)));
}
static void FSReadDir(CPUState* c) { ret(c, (u32)read_dir(arg(c, 2), arg(c, 3))); }
static void FSCloseDir(CPUState* c) { ret(c, (u32)close_dir(arg(c, 2))); }
static void FSRewindDir(CPUState* c) {
    u32 slot = arg(c, 2) - HANDLE_BASE;
    if (slot < 64 && g_dirs[slot].used) g_dirs[slot].next = 0;
    ret(c, FS_OK);
}
static void FSMakeDir(CPUState* c) {
    char path[512];
    guest_string(arg(c, 2), path, sizeof(path));
    ret(c, (u32)make_dir(path));
}
static void FSRemove(CPUState* c) {
    char path[512];
    guest_string(arg(c, 2), path, sizeof(path));
    ret(c, (u32)remove_path(path));
}
static void FSRename(CPUState* c) {      // (client, block, old, new, mask)
    char a[512], b[512], ha[1100], hb[1100];
    bool ea, eb;
    guest_string(arg(c, 2), a, sizeof(a));
    guest_string(arg(c, 3), b, sizeof(b));
    const Mount* ma = resolve(a, ha, sizeof(ha), &ea);
    const Mount* mb = resolve(b, hb, sizeof(hb), &eb);
    if (!ma || !mb || !ma->writable || !mb->writable) { ret(c, (u32)FS_PERMISSION); return; }
    if (!ea) { ret(c, (u32)FS_NOT_FOUND); return; }
    ret(c, rename(ha, hb) == 0 ? FS_OK : (u32)FS_PERMISSION);
}
static void FSChangeDir(CPUState* c) {
    char path[512];
    guest_string(arg(c, 2), path, sizeof(path));
    char norm[512];
    normalize(path, norm, sizeof(norm));
    snprintf(g_cwd, sizeof(g_cwd), "%s", norm);
    ret(c, FS_OK);
}
static void FSGetCwd(CPUState* c) {      // (client, block, buf, size, mask)
    u8* out = wiiu_mem_ptr(arg(c, 2), arg(c, 3) ? arg(c, 3) : 1);
    if (out && arg(c, 3)) snprintf((char*)out, arg(c, 3), "%s", g_cwd);
    ret(c, FS_OK);
}
static void FSGetFreeSpaceSize(CPUState* c) {   // (client, block, path, *u64, mask)
    wiiu_write32(arg(c, 3), 0);
    wiiu_write32(arg(c, 3) + 4, 0x40000000u);   // 1 GB free
    ret(c, FS_OK);
}

// ---------------------------------------------------------------------------
// nn_save: save data per user account (0xFF = shared by all accounts).
// ---------------------------------------------------------------------------
static void save_path(u8 account, u32 rel_addr, char* out, size_t size) {
    char rel[512];
    guest_string(rel_addr, rel, sizeof(rel));
    while (rel[0] == '/') memmove(rel, rel + 1, strlen(rel));
    if (account == 0xFF) snprintf(out, size, "/vol/save/common/%s", rel);
    else snprintf(out, size, "/vol/save/%08x/%s", 0x80000000u | account, rel);
}
static void save_dir_for(u8 account) {
    char p[1100];
    if (account == 0xFF) snprintf(p, sizeof(p), "%s/common", g_saves_dir);
    else snprintf(p, sizeof(p), "%s/%08x", g_saves_dir, 0x80000000u | account);
    host_mkdirs(p);
}
static void SAVEInit(CPUState* c) { save_dir_for(0xFF); save_dir_for(1); ret(c, 0); }
static void SAVEInitSaveDir(CPUState* c) { save_dir_for((u8)arg(c, 0)); ret(c, 0); }
static void SAVEOpenFile(CPUState* c) {  // (client, block, account, path, mode, *handle, mask)
    char path[700], mode[16];
    save_path((u8)arg(c, 2), arg(c, 3), path, sizeof(path));
    guest_string(arg(c, 4), mode, sizeof(mode));
    ret(c, (u32)open_file(path, mode, arg(c, 5)));
}
static void SAVEGetStat(CPUState* c) {   // (client, block, account, path, *stat, mask)
    char path[700];
    save_path((u8)arg(c, 2), arg(c, 3), path, sizeof(path));
    ret(c, (u32)stat_path(path, arg(c, 4)));
}
static void SAVEMakeDir(CPUState* c) {
    char path[700];
    save_path((u8)arg(c, 2), arg(c, 3), path, sizeof(path));
    s32 r = make_dir(path);
    ret(c, (u32)(r == FS_EXISTS ? FS_OK : r));
}
static void SAVEOpenDir(CPUState* c) {   // (client, block, account, path, *handle, mask)
    char path[700];
    save_path((u8)arg(c, 2), arg(c, 3), path, sizeof(path));
    ret(c, (u32)open_dir(path, arg(c, 4)));
}
static void SAVERemove(CPUState* c) {
    char path[700];
    save_path((u8)arg(c, 2), arg(c, 3), path, sizeof(path));
    ret(c, (u32)remove_path(path));
}
static void SAVERename(CPUState* c) {    // (client, block, account, old, new, mask)
    char a[700], b[700], ha[1100], hb[1100];
    bool ea, eb;
    save_path((u8)arg(c, 2), arg(c, 3), a, sizeof(a));
    save_path((u8)arg(c, 2), arg(c, 4), b, sizeof(b));
    resolve(a, ha, sizeof(ha), &ea);
    resolve(b, hb, sizeof(hb), &eb);
    ret(c, ea && rename(ha, hb) == 0 ? FS_OK : (u32)FS_NOT_FOUND);
}
// Paths to another title's shared data (e.g. the system fonts). These live
// in the console's system storage; with --mlc pointing at a Cemu-style
// mlc01 folder they can be found there.
static void SAVEGetSharedDataTitlePath(CPUState* c) {   // (titleId hi, lo, dir, buf, size)
    char dir[256];
    guest_string(arg(c, 2), dir, sizeof(dir));
    u8* out = wiiu_mem_ptr(arg(c, 3), arg(c, 4) ? arg(c, 4) : 1);
    if (out) snprintf((char*)out, arg(c, 4), "/vol/storage_mlc01/sys/title/%08x/%08x/content/%s",
                      arg(c, 0), arg(c, 1), dir);
    ret(c, 0);
}
static void SAVEGetSharedSaveDataPath(CPUState* c) {    // (titleId hi, lo, dir, buf, size)
    char dir[256];
    guest_string(arg(c, 2), dir, sizeof(dir));
    u8* out = wiiu_mem_ptr(arg(c, 3), arg(c, 4) ? arg(c, 4) : 1);
    if (out) snprintf((char*)out, arg(c, 4), "/vol/storage_mlc01/usr/save/%08x/%08x/user/common/%s",
                      arg(c, 0), arg(c, 1), dir);
    ret(c, 0);
}

static const HleEntry g_fs_table[] = {
    { "FSInit", fs_ok },
    { "FSShutdown", fs_ok },
    { "FSAddClient", fs_ok },
    { "FSAddClientEx", fs_ok },
    { "FSDelClient", fs_ok },
    { "FSInitCmdBlock", fs_ok },
    { "FSSetCmdPriority", fs_ok },
    { "FSSetStateChangeNotification", fs_ok },
    { "FSGetVolumeState", FSGetVolumeState },
    { "FSGetLastError", fs_ok },
    { "FSGetLastErrorCodeForViewer", fs_ok },
    { "FSGetErrorCodeForViewer", fs_ok },
    { "FSFlushQuota", fs_ok },
    { "FSCancelCommand", fs_ok },
    { "FSOpenFile", FSOpenFile },
    { "FSCloseFile", FSCloseFile },
    { "FSReadFile", FSReadFile },
    { "FSReadFileWithPos", FSReadFileWithPos },
    { "FSWriteFile", FSWriteFile },
    { "FSSetPosFile", FSSetPosFile },
    { "FSGetPosFile", FSGetPosFile },
    { "FSIsEof", FSIsEof },
    { "FSGetStatFile", FSGetStatFile },
    { "FSFlushFile", FSFlushFile },
    { "FSTruncateFile", fs_ok },
    { "FSGetStat", FSGetStat },
    { "FSOpenDir", FSOpenDir },
    { "FSReadDir", FSReadDir },
    { "FSCloseDir", FSCloseDir },
    { "FSRewindDir", FSRewindDir },
    { "FSMakeDir", FSMakeDir },
    { "FSRemove", FSRemove },
    { "FSRename", FSRename },
    { "FSChangeDir", FSChangeDir },
    { "FSGetCwd", FSGetCwd },
    { "FSGetFreeSpaceSize", FSGetFreeSpaceSize },

    { "SAVEInit", SAVEInit },
    { "SAVEShutdown", fs_ok },
    { "SAVEInitSaveDir", SAVEInitSaveDir },
    { "SAVEOpenFile", SAVEOpenFile },
    { "SAVEGetStat", SAVEGetStat },
    { "SAVEMakeDir", SAVEMakeDir },
    { "SAVEOpenDir", SAVEOpenDir },
    { "SAVERemove", SAVERemove },
    { "SAVERename", SAVERename },
    { "SAVEFlushQuota", fs_ok },
    { "SAVEGetSharedDataTitlePath", SAVEGetSharedDataTitlePath },
    { "SAVEGetSharedSaveDataPath", SAVEGetSharedSaveDataPath },
};

const HleEntry* fs_hle_table(size_t* count) {
    *count = sizeof(g_fs_table) / sizeof(g_fs_table[0]);
    return g_fs_table;
}
