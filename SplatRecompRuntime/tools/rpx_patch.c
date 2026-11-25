// rpx_patch: fill in a Wii U .rpx's import addresses ahead of time.
//
// Nintendo's .rpx files leave the addresses of system-library imports blank
// in the game's code; the console's loader fills them in at boot. A static
// recompiler bakes the code in as-is, so we fill them in BEFORE recompiling:
//
//   rpx_patch <input.rpx> <output.rpx>
//
// The output is the same program with every section unpacked and every
// relocation that points at an import resolved:
//   - function imports  -> the import's own address (the runtime catches
//                          jumps there and runs its PC version)
//   - data imports      -> a slot from rpx_imports_layout.h, which the
//                          runtime fills in
// Branches through trampolines are left alone; the runtime handles those.
//
// The output is derived from the game, so keep it out of any repository.
#include "rpx_imports_layout.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;

#define SHT_SYMTAB 2u
#define SHT_RELA 4u
#define SHT_NOBITS 8u
#define SHF_EXECINSTR 4u
#define SHF_RPL_ZLIB 0x08000000u

typedef struct { u32 name, type, flags, addr, offset, size, link, info, align, entsize; } Shdr;
typedef struct { u8* data; u32 size; } Blob;

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
static void put32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void put16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

static Blob unpack(const u8* file, u32 fsize, const Shdr* s) {
    Blob b = { NULL, 0 };
    if (s->type == SHT_NOBITS || s->size == 0 || s->offset > fsize || s->size > fsize - s->offset)
        return b;
    const u8* raw = file + s->offset;
    if (!(s->flags & SHF_RPL_ZLIB)) {
        b.data = malloc(s->size);
        memcpy(b.data, raw, s->size);
        b.size = s->size;
        return b;
    }
    u32 n = be32(raw);
    b.data = malloc(n ? n : 1);
    uLongf len = n;
    if (uncompress(b.data, &len, raw + 4, s->size - 4) != Z_OK || len != n) {
        free(b.data);
        b.data = NULL;
        return b;
    }
    b.size = n;
    return b;
}

static const char* str_at(const Blob* t, u32 off) {
    return (t->data && off < t->size) ? (const char*)t->data + off : "";
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: rpx_patch <input.rpx> <output.rpx>\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "error: can't open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    u32 fsize = (u32)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* file = malloc(fsize);
    if (fread(file, 1, fsize, f) != fsize) { fprintf(stderr, "error: read failed\n"); return 1; }
    fclose(f);
    if (fsize < 52 || memcmp(file, "\x7f" "ELF", 4) != 0) { fprintf(stderr, "error: not an .rpx\n"); return 1; }

    u32 shoff = be32(file + 32);
    u16 shnum = be16(file + 48), shstrndx = be16(file + 50);
    Shdr* sh = calloc(shnum, sizeof(Shdr));
    Blob* data = calloc(shnum, sizeof(Blob));
    for (u16 i = 0; i < shnum; i++) {
        const u8* p = file + shoff + i * 40u;
        u32* fld = (u32*)&sh[i];
        for (int k = 0; k < 10; k++) fld[k] = be32(p + k * 4);
    }
    for (u16 i = 0; i < shnum; i++) data[i] = unpack(file, fsize, &sh[i]);
    const Blob* names = &data[shstrndx];

    // Number every data-import symbol in symbol-table order (must match the
    // runtime's numbering in rpx_loader.c).
    u32 sym_tab = 0xFFFF;
    for (u16 i = 0; i < shnum; i++) if (sh[i].type == SHT_SYMTAB) { sym_tab = i; break; }
    if (sym_tab == 0xFFFF) { fprintf(stderr, "error: no symbol table\n"); return 1; }
    const Blob* syms = &data[sym_tab];
    const Blob* strs = &data[sh[sym_tab].link];
    u32 nsyms = syms->size / 16;
    u32* resolved = calloc(nsyms ? nsyms : 1, sizeof(u32));
    u8* is_import = calloc(nsyms ? nsyms : 1, 1);
    u32 dimport_index = 0;
    for (u32 i = 0; i < nsyms; i++) {
        const u8* s = syms->data + i * 16;
        u16 shndx = be16(s + 14);
        u8 type = s[12] & 0xF;
        if (shndx == 0 || shndx >= shnum || type == 3 || !str_at(strs, be32(s))[0]) continue;
        const char* sec = str_at(names, sh[shndx].name);
        if (!strncmp(sec, ".fimport_", 9)) {
            is_import[i] = 1;
            resolved[i] = be32(s + 4);
        } else if (!strncmp(sec, ".dimport_", 9)) {
            is_import[i] = 2;
            resolved[i] = dimport_object_address(dimport_index++);
        }
    }

    // Apply relocations that point at imports.
    u32 patched_lo = 0, patched_hi = 0, patched_ha = 0, patched_32 = 0, skipped_rel24 = 0, other = 0;
    for (u16 r = 0; r < shnum; r++) {
        if (sh[r].type != SHT_RELA || sh[r].info >= shnum || sh[r].link != sym_tab) continue;
        const Shdr* tgt = &sh[sh[r].info];
        Blob* td = &data[sh[r].info];
        const Blob* rel = &data[r];
        for (u32 off = 0; rel->data && off + 12 <= rel->size; off += 12) {
            u32 where = be32(rel->data + off);
            u32 info = be32(rel->data + off + 4);
            u32 addend = be32(rel->data + off + 8);
            u32 type = info & 0xFF, si = info >> 8;
            if (si >= nsyms || !is_import[si]) continue;
            if (where < tgt->addr || where - tgt->addr + 4 > td->size) continue;
            u8* p = td->data + (where - tgt->addr);
            u32 v = resolved[si] + addend;
            switch (type) {
            case 1:  put32(p, v); patched_32++; break;                               // ADDR32
            case 4:  put16(p, (u16)v); patched_lo++; break;                          // ADDR16_LO
            case 5:  put16(p, (u16)(v >> 16)); patched_hi++; break;                  // ADDR16_HI
            case 6:  put16(p, (u16)((v + 0x8000u) >> 16)); patched_ha++; break;      // ADDR16_HA
            case 10: skipped_rel24++; break;                                         // REL24 (trampolines)
            default: other++; break;
            }
        }
        (void)tgt;
    }
    printf("patched %u lo + %u hi + %u ha half-words and %u pointers; "
           "%u trampoline branches left for the runtime; %u other\n",
           patched_lo, patched_hi, patched_ha, patched_32, skipped_rel24, other);
    printf("data imports: %u (placed from 0x%08X)\n", dimport_index, DIMPORT_OBJECT_BASE);

    // Write the output: same headers, every section stored unpacked.
    FILE* out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "error: can't write %s\n", argv[2]); return 1; }
    u8 header[52];
    memcpy(header, file, 52);
    u32 pos = 52;
    fwrite(header, 1, 52, out);
    for (u16 i = 0; i < shnum; i++) {
        if (sh[i].type == SHT_NOBITS || !data[i].data) { sh[i].offset = 0; continue; }
        while (pos % 64) { fputc(0, out); pos++; }
        sh[i].offset = pos;
        sh[i].size = data[i].size;
        sh[i].flags &= ~SHF_RPL_ZLIB;
        fwrite(data[i].data, 1, data[i].size, out);
        pos += data[i].size;
    }
    while (pos % 64) { fputc(0, out); pos++; }
    u32 new_shoff = pos;
    for (u16 i = 0; i < shnum; i++) {
        u8 buf[40];
        const u32* fld = (const u32*)&sh[i];
        for (int k = 0; k < 10; k++) put32(buf + k * 4, fld[k]);
        fwrite(buf, 1, 40, out);
    }
    fseek(out, 32, SEEK_SET);
    u8 sb[4];
    put32(sb, new_shoff);
    fwrite(sb, 1, 4, out);
    fclose(out);
    printf("wrote %s\n", argv[2]);
    return 0;
}
