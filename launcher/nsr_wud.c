// nsr_wud: reads a Wii U disc image (.wud, or .wux compressed) with the player's own keys and
// extracts the game partition (code\, content\, meta\) as plain files, the way a dump looks.
//
//   nsr_wud info    <image> [keys.txt ...]
//   nsr_wud extract <image> <out folder> [keys.txt ...]
//
// Keys: every 32-hex line of the keys.txt files given, else keys.txt next to the image, else
// Cemu's (%APPDATA%\Cemu\keys.txt, %LOCALAPPDATA%\Cemu\keys.txt). Each key is tried as the
// disc key until the partition table decrypts, then each as the common key until the game
// partition's title key (from its ticket in the SI partition) decrypts the FST. No key is in
// this program. The image is only read.
//
// Format (as the console reads it; the layout is public):
//   WUX: "WUX0" + 0x1099d02e, sector size, uncompressed size; a table of 32-bit sector numbers,
//        then sectors (the table is a dictionary: logical sector i lives at physical slot table[i]).
//   WUD: 0x18000 partition table (0x8000 bytes, AES-128-CBC with the disc key, IV 0): magic
//        0xCCA6E67B, block size 0x8000 at +4, number of partitions at +0x1C, entries at +0x800
//        (0x80 each: name 31 bytes, address count at +0x1F, address in 0x8000 sectors at +0x20).
//        A partition's header (plain, 0x60 bytes) at its address: FST size at +0x14, FST sector
//        at +0x18. The FST (AES-CBC, IV 0): "FST\0", offset factor, cluster count, 0x20 clusters
//        (sector offset, size), 0x10 entries (type:8 name:24, offset, size, flags:16 cluster:16),
//        then names. Cluster data: raw clusters are one CBC stream in 0x8000 blocks (IV = the
//        cluster index in the first two bytes); hashed clusters are 0x10000 blocks of 0x400 hash
//        bytes (CBC, IV 0) + 0xFC00 data bytes (CBC, IV = that block's H0 hash entry).
//        The SI partition (disc key) holds <gm index>\title.tik: the title key, encrypted with the
//        common key (CBC, IV = title id + zeros) at +0x1BF, title id at +0x1DC.
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <direct.h>
#include <sys/stat.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

// ---- AES-128 decryption (the standard inverse cipher) --------------------------------------------
static const u8 SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };
static u8 INV_SBOX[256];
typedef struct { u8 rk[176]; } Aes;
static u8 xtime(u8 x) { return (u8)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static u8 gmul(u8 a, u8 b) { u8 r = 0; while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; } return r; }
static void aes_init(Aes* a, const u8 key[16]) {
    static bool inv_ready = false;
    if (!inv_ready) { for (int i = 0; i < 256; i++) INV_SBOX[SBOX[i]] = (u8)i; inv_ready = true; }
    memcpy(a->rk, key, 16);
    u8 rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        u8 t[4]; memcpy(t, a->rk + i - 4, 4);
        if (i % 16 == 0) { u8 x = t[0]; t[0] = SBOX[t[1]] ^ rcon; t[1] = SBOX[t[2]]; t[2] = SBOX[t[3]]; t[3] = SBOX[x]; rcon = xtime(rcon); }
        for (int j = 0; j < 4; j++) a->rk[i + j] = a->rk[i - 16 + j] ^ t[j];
    }
}
static void aes_decrypt_block(const Aes* a, const u8 in[16], u8 out[16]) {
    u8 s[16]; memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= a->rk[160 + i];
    for (int round = 9; round >= 0; round--) {
        // inverse shift rows
        u8 t[16]; memcpy(t, s, 16);
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[((c + r) % 4) * 4 + r] = s[c * 4 + r];
        // inverse sub bytes
        for (int i = 0; i < 16; i++) t[i] = INV_SBOX[t[i]];
        // add round key
        for (int i = 0; i < 16; i++) t[i] ^= a->rk[round * 16 + i];
        if (round > 0) {   // inverse mix columns
            for (int c = 0; c < 4; c++) {
                u8* col = t + c * 4;
                u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                col[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                col[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                col[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        }
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}
// CBC decrypt in place; `iv` is updated to the last ciphertext block (so a stream can continue)
static void aes_cbc_decrypt(const Aes* a, u8* data, size_t n, u8 iv[16]) {
    u8 prev[16], cur[16], out[16];
    memcpy(prev, iv, 16);
    for (size_t i = 0; i + 16 <= n; i += 16) {
        memcpy(cur, data + i, 16);
        aes_decrypt_block(a, cur, out);
        for (int j = 0; j < 16; j++) data[i + j] = out[j] ^ prev[j];
        memcpy(prev, cur, 16);
    }
    memcpy(iv, prev, 16);
}

// ---- the image (WUD / WUX) -------------------------------------------------------------------------
typedef struct {
    FILE* f;
    bool wux;
    u64 size;              // uncompressed size
    u32 sector;            // WUX sector size
    u32* index; u64 nindex;
    u64 sectors_at;        // WUX: where the sector slots start
} Image;
static bool img_open(Image* im, const char* path) {
    memset(im, 0, sizeof(*im));
    im->f = fopen(path, "rb");
    if (!im->f) return false;
    _fseeki64(im->f, 0, SEEK_END);
    u64 fsize = (u64)_ftelli64(im->f);
    _fseeki64(im->f, 0, SEEK_SET);
    u8 h[32];
    if (fread(h, 1, 32, im->f) != 32) return false;
    u32 m0, m1, sec; u64 usize;
    memcpy(&m0, h, 4); memcpy(&m1, h + 4, 4); memcpy(&sec, h + 8, 4); memcpy(&usize, h + 12, 8);   // (little-endian fields)
    if (m0 == 0x30585557u && m1 == 0x1099d02eu) {
        if (sec < 0x100 || sec >= 0x10000000u) return false;
        im->wux = true; im->sector = sec; im->size = usize;
        im->nindex = (usize + sec - 1) / sec;
        im->index = (u32*)malloc(im->nindex * 4);
        _fseeki64(im->f, 24, SEEK_SET);   // (the header is 24 bytes: two magics, sector size, size (8), flags)
        if (fread(im->index, 4, im->nindex, im->f) != im->nindex) return false;
        u64 table_end = 24 + im->nindex * 4;
        im->sectors_at = (table_end + sec - 1) / sec * sec;
    } else {
        im->wux = false; im->size = fsize;
    }
    return true;
}
static bool img_read(Image* im, u64 off, void* dst, u64 n) {
    if (off + n > im->size) return false;
    u8* p = (u8*)dst;
    if (!im->wux) { _fseeki64(im->f, (long long)off, SEEK_SET); return fread(p, 1, n, im->f) == n; }
    while (n) {
        u64 li = off / im->sector, so = off % im->sector;
        u64 take = im->sector - so; if (take > n) take = n;
        if (li >= im->nindex) return false;
        u64 phys = im->sectors_at + (u64)im->index[li] * im->sector + so;
        _fseeki64(im->f, (long long)phys, SEEK_SET);
        if (fread(p, 1, take, im->f) != take) return false;
        p += take; off += take; n -= take;
    }
    return true;
}

// ---- keys --------------------------------------------------------------------------------------------
static u8 g_keys[256][16]; static int g_nkeys;
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int keys_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char line[512]; int added = 0;
    while (fgets(line, sizeof(line), f)) {
        char* c = strpbrk(line, "#;\r\n"); if (c) *c = 0;   // (comments; a key per line; spaces, dashes allowed)
        char hex[64]; int n = 0;
        for (char* p = line; *p && n < 63; p++) if (*p != ' ' && *p != '\t' && *p != '-' && *p != '_') hex[n++] = *p;
        if (n != 32 || g_nkeys >= 256) continue;
        u8 k[16]; bool ok = true;
        for (int i = 0; i < 16; i++) { int a = hexv(hex[i * 2]), b = hexv(hex[i * 2 + 1]); if (a < 0 || b < 0) { ok = false; break; } k[i] = (u8)(a * 16 + b); }
        if (!ok) continue;
        bool dup = false;
        for (int i = 0; i < g_nkeys; i++) if (!memcmp(g_keys[i], k, 16)) dup = true;
        if (!dup) { memcpy(g_keys[g_nkeys++], k, 16); added++; }
    }
    fclose(f);
    return added;
}
static void keys_find(const char* image, int argc, char** argv, int first) {
    for (int i = first; i < argc; i++) { int n = keys_load(argv[i]); printf("keys: %d from %s\n", n, argv[i]); }
    char p[1024];
    snprintf(p, sizeof(p), "%s", image);
    char* s = strrchr(p, '\\'); if (!s) s = strrchr(p, '/');
    if (s) { strcpy(s + 1, "keys.txt"); int n = keys_load(p); if (n) printf("keys: %d from %s\n", n, p); }
    const char* env[2] = { "APPDATA", "LOCALAPPDATA" };
    for (int i = 0; i < 2; i++) { const char* e = getenv(env[i]); if (!e) continue; snprintf(p, sizeof(p), "%s\\Cemu\\keys.txt", e); int n = keys_load(p); if (n) printf("keys: %d from %s\n", n, p); }
}

// ---- the disc -------------------------------------------------------------------------------------
static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u16 be16(const u8* p) { return (u16)(((u16)p[0] << 8) | p[1]); }
#define SECTOR 0x8000ull
typedef struct { u32 offset, size; u8 hash_mode; } Cluster;
typedef struct { u8 type; u32 name_off, offset, size; u16 flags, cluster; } Entry;
typedef struct {
    u64 base;                    // partition start (bytes)
    Aes key;                     // the partition's key
    u32 offset_factor;
    Cluster* clusters; u32 ncluster;
    Entry* entries; u32 nentry;
    const char* names;
    u8* fst; u32 fst_size;
} Part;
static bool fst_parse(Part* P, u8* fst, u32 size) {
    if (size < 0x20 || be32(fst) != 0x46535400u) return false;
    P->fst = fst; P->fst_size = size;
    P->offset_factor = be32(fst + 4);
    P->ncluster = be32(fst + 8);
    if (P->ncluster >= 0x1000 || 0x20 + P->ncluster * 0x20 + 0x10 > size) return false;
    P->clusters = (Cluster*)calloc(P->ncluster, sizeof(Cluster));
    for (u32 i = 0; i < P->ncluster; i++) {
        const u8* c = fst + 0x20 + i * 0x20;
        P->clusters[i].offset = be32(c); P->clusters[i].size = be32(c + 4); P->clusters[i].hash_mode = c[0x14];
    }
    const u8* e0 = fst + 0x20 + P->ncluster * 0x20;
    P->nentry = be32(e0 + 8);   // (the root directory's end index = the entry count)
    if ((u64)(e0 - fst) + (u64)P->nentry * 0x10 > size) return false;
    P->entries = (Entry*)calloc(P->nentry, sizeof(Entry));
    for (u32 i = 0; i < P->nentry; i++) {
        const u8* e = e0 + i * 0x10;
        u32 tn = be32(e);
        P->entries[i].type = (u8)(tn >> 24); P->entries[i].name_off = tn & 0xFFFFFF;
        P->entries[i].offset = be32(e + 4); P->entries[i].size = be32(e + 8);
        P->entries[i].flags = be16(e + 12); P->entries[i].cluster = be16(e + 14);
    }
    P->names = (const char*)(e0 + P->nentry * 0x10);
    return true;
}
static const char* entry_name(const Part* P, u32 i) {
    const char* n = P->names + P->entries[i].name_off;
    return (const u8*)n < P->fst + P->fst_size ? n : "";
}
// Reads `len` bytes of a file that starts at `file_off` (in the cluster's bytes) from the cluster.
static bool cluster_read(Image* im, Part* P, u32 ci, u64 pos, u8* dst, u64 len) {
    if (ci >= P->ncluster) return false;
    const Cluster* c = &P->clusters[ci];
    u64 cbase = P->base + (u64)c->offset * SECTOR;
    if (c->hash_mode == 2) {   // hashed: 0x10000 blocks of 0x400 hashes + 0xFC00 data
        static u8 blk[0x10000];
        while (len) {
            u64 bi = pos / 0xFC00, in = pos % 0xFC00;
            if (!img_read(im, cbase + bi * 0x10000, blk, 0x10000)) return false;
            u8 iv[16] = { 0 };
            aes_cbc_decrypt(&P->key, blk, 0x400, iv);
            memcpy(iv, blk + 20 * (bi % 16), 16);
            aes_cbc_decrypt(&P->key, blk + 0x400, 0xFC00, iv);
            u64 take = 0xFC00 - in; if (take > len) take = len;
            memcpy(dst, blk + 0x400 + in, take);
            dst += take; pos += take; len -= take;
        }
        return true;
    }
    // raw: one CBC stream over the cluster in 0x8000 blocks; the IV is the previous ciphertext block
    static u8 blk[0x8000];
    while (len) {
        u64 bi = pos / SECTOR, in = pos % SECTOR;
        if (!img_read(im, cbase + bi * SECTOR, blk, SECTOR)) return false;
        u8 iv[16] = { 0 };
        if (bi == 0) { iv[0] = (u8)(ci >> 8); iv[1] = (u8)ci; }
        else if (!img_read(im, cbase + bi * SECTOR - 16, iv, 16)) return false;
        aes_cbc_decrypt(&P->key, blk, SECTOR, iv);
        u64 take = SECTOR - in; if (take > len) take = len;
        memcpy(dst, blk + in, take);
        dst += take; pos += take; len -= take;
    }
    return true;
}
static bool part_open(Image* im, Part* P, u64 base, const u8 key[16]) {
    memset(P, 0, sizeof(*P));
    P->base = base;
    aes_init(&P->key, key);
    u8 hdr[0x60];
    if (!img_read(im, base, hdr, 0x60)) return false;
    u32 fst_size = be32(hdr + 0x14), fst_sector = be32(hdr + 0x18);
    if (fst_size < 0x20 || fst_size > (64u << 20)) return false;
    u32 n = (fst_size + 15) & ~15u;
    u8* fst = (u8*)malloc(n);
    if (!img_read(im, base + (u64)fst_sector * SECTOR, fst, n)) { free(fst); return false; }
    u8 iv[16] = { 0 };
    aes_cbc_decrypt(&P->key, fst, n, iv);
    if (!fst_parse(P, fst, fst_size)) { free(fst); return false; }
    return true;
}
static int find_entry(const Part* P, const char* path) {   // "dir/file" from the root
    u32 dir = 0, end = P->nentry;
    const char* p = path;
    while (*p) {
        const char* q = strchr(p, '/');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        int found = -1;
        for (u32 i = dir + 1; i < end; ) {
            const Entry* e = &P->entries[i];
            const char* nm = entry_name(P, i);
            if (strlen(nm) == len && !_strnicmp(nm, p, len)) { found = (int)i; break; }
            i = (e->type & 1) ? e->size : i + 1;   // (skip a directory's contents)
        }
        if (found < 0) return -1;
        if (!q) return found;
        if (!(P->entries[found].type & 1)) return -1;
        dir = (u32)found; end = P->entries[found].size; p = q + 1;
    }
    return -1;
}
static bool read_file(Image* im, Part* P, int ei, u8** out, u32* size) {
    const Entry* e = &P->entries[ei];
    *size = e->size;
    *out = (u8*)malloc(e->size ? e->size : 1);
    return cluster_read(im, P, e->cluster, (u64)e->offset * P->offset_factor, *out, e->size);
}

// ---- opening the disc with the keys -------------------------------------------------------------
typedef struct { u8 table[0x8000]; int disc_key; u32 npart; u64 si_base, gm_base; int gm_index; } Disc;
static bool disc_open(Image* im, Disc* D) {
    memset(D, 0, sizeof(*D));
    D->disc_key = -1;
    u8 raw[0x8000];
    if (!img_read(im, 0x18000, raw, 0x8000)) { printf("NSR-WUD-FAIL the image is too small to be a Wii U disc\n"); return false; }
    for (int k = 0; k < g_nkeys; k++) {
        Aes a; aes_init(&a, g_keys[k]);
        memcpy(D->table, raw, 0x8000);
        u8 iv[16] = { 0 };
        aes_cbc_decrypt(&a, D->table, 0x20, iv);
        if (be32(D->table) == 0xCCA6E67Bu && be32(D->table + 4) == 0x8000) {
            memcpy(D->table, raw, 0x8000);
            memset(iv, 0, 16);
            aes_cbc_decrypt(&a, D->table, 0x8000, iv);
            D->disc_key = k;
            break;
        }
    }
    if (D->disc_key < 0) { printf("NSR-WUD-FAIL none of the %d keys decrypts this disc's partition table (the disc key for this game isn't in keys.txt)\n", g_nkeys); return false; }
    D->npart = be32(D->table + 0x1C);
    if (D->npart > 30) { printf("NSR-WUD-FAIL odd partition count %u\n", D->npart); return false; }
    D->gm_index = -1;
    for (u32 i = 0; i < D->npart; i++) {
        const u8* e = D->table + 0x800 + i * 0x80;
        char name[32]; memcpy(name, e, 31); name[31] = 0;
        u64 addr = (u64)be32(e + 0x20) * SECTOR;
        printf("partition %u: %s at 0x%llx\n", i, name, (unsigned long long)addr);
        if (!strncmp(name, "SI", 2) && !D->si_base) D->si_base = addr;
        if (!strncmp(name, "GM", 2) && D->gm_index < 0) { D->gm_base = addr; D->gm_index = (int)i; }
    }
    if (!D->si_base || D->gm_index < 0) { printf("NSR-WUD-FAIL no SI / GM partition\n"); return false; }
    return true;
}
// the game partition's key: its ticket in the SI partition, decrypted with whichever key works as the common key
static bool game_key(Image* im, Disc* D, u8 out[16]) {
    Part si;
    if (!part_open(im, &si, D->si_base, g_keys[D->disc_key])) { printf("NSR-WUD-FAIL the SI partition's file table didn't decrypt\n"); return false; }
    char path[32]; snprintf(path, sizeof(path), "%02x/title.tik", D->gm_index);
    int ei = find_entry(&si, path);
    if (ei < 0) { printf("NSR-WUD-FAIL %s not found in the SI partition\n", path); return false; }
    u8* tik; u32 tsize;
    if (!read_file(im, &si, ei, &tik, &tsize) || tsize < 0x220) { printf("NSR-WUD-FAIL couldn't read the ticket\n"); return false; }
    u8 enc[16]; memcpy(enc, tik + 0x1BF, 16);
    u8 tid[8]; memcpy(tid, tik + 0x1DC, 8);
    printf("ticket: title %02x%02x%02x%02x%02x%02x%02x%02x\n", tid[0], tid[1], tid[2], tid[3], tid[4], tid[5], tid[6], tid[7]);
    // which key is the common key? the one whose title key makes the GM file table start with "FST"
    u8 hdr_raw[16];
    if (!img_read(im, D->gm_base, hdr_raw, 16)) return false;
    u8 ph[0x60]; img_read(im, D->gm_base, ph, 0x60);
    u32 fst_sector = be32(ph + 0x18);
    u8 fst0[16]; if (!img_read(im, D->gm_base + (u64)fst_sector * SECTOR, fst0, 16)) return false;
    for (int k = 0; k < g_nkeys; k++) {
        Aes c; aes_init(&c, g_keys[k]);
        u8 tk[16]; memcpy(tk, enc, 16);
        u8 iv[16] = { 0 }; memcpy(iv, tid, 8);
        aes_cbc_decrypt(&c, tk, 16, iv);
        Aes t; aes_init(&t, tk);
        u8 probe[16]; memcpy(probe, fst0, 16);
        u8 z[16] = { 0 };
        aes_cbc_decrypt(&t, probe, 16, z);
        if (be32(probe) == 0x46535400u) { memcpy(out, tk, 16); printf("common key: entry %d of keys.txt works\n", k + 1); free(tik); return true; }
    }
    free(tik);
    printf("NSR-WUD-FAIL none of the %d keys works as the common key for this ticket\n", g_nkeys);
    return false;
}

// ---- extraction ---------------------------------------------------------------------------------------
static void mkdirs(const char* path) {
    char p[1024]; snprintf(p, sizeof(p), "%s", path);
    for (char* s = p + 3; *s; s++) if (*s == '\\' || *s == '/') { char c = *s; *s = 0; _mkdir(p); *s = c; }
    _mkdir(p);
}
static u64 g_total_bytes, g_done_bytes; static u32 g_files, g_done_files;
volatile int nsr_wud_cancel;   // set by the launcher (Cancel / closing the window): the extract stops at the next 1 MB
static bool extract_dir(Image* im, Part* P, u32 dir, u32 end, const char* out) {
    for (u32 i = dir + 1; i < end; ) {
        const Entry* e = &P->entries[i];
        char path[1024]; snprintf(path, sizeof(path), "%s\\%s", out, entry_name(P, i));
        if (e->type & 1) {
            mkdirs(path);
            if (!extract_dir(im, P, i, e->size, path)) return false;
            i = e->size;
            continue;
        }
        if (!(e->type & 0x80)) {   // (0x80 = a link / deleted entry: skipped)
            static u8 buf[1 << 20];
            FILE* f = fopen(path, "wb");
            if (!f) { printf("NSR-WUD-FAIL can't write %s\n", path); return false; }
            u64 pos = (u64)e->offset * P->offset_factor, left = e->size;
            while (left) {
                if (nsr_wud_cancel) { fclose(f); printf("NSR-WUD-FAIL cancelled\n"); return false; }
                u64 take = left < sizeof(buf) ? left : sizeof(buf);
                if (!cluster_read(im, P, e->cluster, pos, buf, take)) { fclose(f); printf("NSR-WUD-FAIL read error in %s\n", path); return false; }
                fwrite(buf, 1, take, f);
                pos += take; left -= take; g_done_bytes += take;
            }
            fclose(f);
            g_done_files++;
            printf("NSR-WUD %u/%u %.0f%% %s\n", g_done_files, g_files, g_total_bytes ? 100.0 * g_done_bytes / g_total_bytes : 0.0, entry_name(P, i));
            fflush(stdout);
        }
        i++;
    }
    return true;
}
static void count_dir(Part* P, u32 dir, u32 end) {
    for (u32 i = dir + 1; i < end; ) {
        const Entry* e = &P->entries[i];
        if (e->type & 1) { count_dir(P, i, e->size); i = e->size; continue; }
        if (!(e->type & 0x80)) { g_files++; g_total_bytes += e->size; }
        i++;
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: nsr_wud info <image> [keys.txt ...]\n       nsr_wud extract <image> <out folder> [keys.txt ...]\n"); return 1; }
    bool extract = !strcmp(argv[1], "extract");
    const char* image = argv[2];
    const char* out = extract ? (argc > 3 ? argv[3] : NULL) : NULL;
    if (extract && !out) { printf("NSR-WUD-FAIL no output folder\n"); return 1; }
    keys_find(image, argc, argv, extract ? 4 : 3);
    if (!g_nkeys) { printf("NSR-WUD-FAIL no keys: put your keys.txt next to the image (one 32-hex key per line), or give its path\n"); return 2; }
    Image im;
    if (!img_open(&im, image)) { printf("NSR-WUD-FAIL can't open %s as a .wud/.wux image\n", image); return 2; }
    printf("image: %s, %.2f GB%s\n", image, im.size / 1073741824.0, im.wux ? " (WUX compressed)" : "");
    Disc D;
    if (!disc_open(&im, &D)) return 3;
    u8 gk[16];
    if (!game_key(&im, &D, gk)) return 3;
    Part gm;
    if (!part_open(&im, &gm, D.gm_base, gk)) { printf("NSR-WUD-FAIL the game partition's file table didn't decrypt\n"); return 3; }
    count_dir(&gm, 0, gm.nentry);
    printf("game partition: %u files, %.2f GB, %u clusters\n", g_files, g_total_bytes / 1073741824.0, gm.ncluster);
    if (!extract) {
        int mi = find_entry(&gm, "meta/meta.xml");
        if (mi >= 0) {
            u8* x; u32 n;
            if (read_file(&im, &gm, mi, &x, &n)) { char* s = (char*)realloc(x, n + 1); s[n] = 0; char* v = strstr(s, "<title_version"); char* id = strstr(s, "<title_id"); if (id) { char* q = strchr(id, '>'); printf("meta: title_id %.16s\n", q ? q + 1 : "?"); } if (v) { char* q = strchr(v, '>'); printf("meta: title_version %.*s\n", (int)strcspn(q ? q + 1 : "", "<"), q ? q + 1 : ""); } free(s); }
        }
        return 0;
    }
    mkdirs(out);
    if (!extract_dir(&im, &gm, 0, gm.nentry, out)) return 4;
    printf("NSR-WUD-DONE %u files, %.2f GB in %s\n", g_done_files, g_done_bytes / 1073741824.0, out);
    return 0;
}
