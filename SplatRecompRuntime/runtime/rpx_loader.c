#include "rpx_loader.h"
#include "wiiu_mem.h"
#include "rpx_imports_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SHT_SYMTAB    2u
#define SHT_RELA      4u
#define R_PPC_ADDR32     1u
#define R_PPC_ADDR16_LO  4u
#define R_PPC_ADDR16_HI  5u
#define R_PPC_ADDR16_HA  6u
#define R_PPC_REL24      10u
#define SHT_RPL_FILEINFO 0x80000004u
#define SHT_NOBITS    8u
#define SHF_ALLOC     0x2u
#define SHF_RPL_ZLIB  0x08000000u

typedef struct {
    u32 name, type, flags, addr, offset, size, link, info, align, entsize;
} Shdr;

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

static u8* read_file(const char* path, u32* size_out) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* data = size > 0 ? (u8*)malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); data = NULL; }
    fclose(f);
    *size_out = (u32)size;
    return data;
}

// Returns a malloc'd copy of a section's real (decompressed) bytes.
static u32 g_debug_addr[8];
static int g_debug_count = 0;
void rpx_debug_address(u32 addr) { if (g_debug_count < 8) g_debug_addr[g_debug_count++] = addr; }

static u8* section_bytes(const u8* file, u32 file_size, const Shdr* s, u32* out_size) {
    *out_size = 0;
    if (s->type == SHT_NOBITS || s->size == 0) return NULL;
    if (s->offset > file_size || s->size > file_size - s->offset) return NULL;
    const u8* raw = file + s->offset;
    if (!(s->flags & SHF_RPL_ZLIB)) {
        u8* copy = (u8*)malloc(s->size);
        memcpy(copy, raw, s->size);
        *out_size = s->size;
        return copy;
    }
    u32 inflated = be32(raw);
    u8* out = (u8*)malloc(inflated ? inflated : 1);
    uLongf len = inflated;
    if (uncompress(out, &len, raw + 4, s->size - 4) != Z_OK || len != inflated) {
        free(out);
        return NULL;
    }
    *out_size = inflated;
    return out;
}

static const char* str_at(const u8* table, u32 table_size, u32 off) {
    if (!table || off >= table_size) return "";
    return (const char*)table + off;
}

static int compare_symbol(const void* a, const void* b);

bool rpx_load_into_memory(const char* path, RpxImage* img) {
    memset(img, 0, sizeof(*img));
    u32 fsize = 0;
    u8* file = read_file(path, &fsize);
    if (!file || fsize < 52 || memcmp(file, "\x7f" "ELF", 4) != 0) {
        fprintf(stderr, "error: can't read '%s' as an .rpx\n", path);
        free(file);
        return false;
    }

    img->entry_point = be32(file + 24);
    u32 shoff = be32(file + 32);
    u16 shnum = be16(file + 48);
    u16 shstrndx = be16(file + 50);
    if (shoff > fsize || (u64)shnum * 40 > fsize - shoff || shstrndx >= shnum) {
        fprintf(stderr, "error: bad section table in '%s'\n", path);
        free(file);
        return false;
    }

    Shdr* sh = (Shdr*)calloc(shnum, sizeof(Shdr));
    for (u16 i = 0; i < shnum; i++) {
        const u8* p = file + shoff + i * 40u;
        u32* f = (u32*)&sh[i];
        for (int k = 0; k < 10; k++) f[k] = be32(p + k * 4);
    }

    u32 names_size = 0;
    u8* names = section_bytes(file, fsize, &sh[shstrndx], &names_size);

    // 1. Copy every loadable section to its address.
    for (u16 i = 0; i < shnum; i++) {
        const Shdr* s = &sh[i];
        if (img->section_count < RPX_MAX_SECTIONS) {
            RpxSection* rs = &img->sections[img->section_count++];
            snprintf(rs->name, sizeof(rs->name), "%s", str_at(names, names_size, s->name));
            rs->type = s->type; rs->flags = s->flags; rs->address = s->addr;
            rs->size = s->type == SHT_NOBITS ? s->size : 0;
        }
        if (!(s->flags & SHF_ALLOC) || s->addr == 0) continue;
        u32 size = 0;
        u8* bytes = section_bytes(file, fsize, s, &size);
        u32 mem_size = s->type == SHT_NOBITS ? s->size : size;
        u8* dst = wiiu_mem_ptr(s->addr, mem_size ? mem_size : 1);
        if (!dst) {
            fprintf(stderr, "warn: section '%s' at 0x%08X doesn't fit guest memory\n",
                    str_at(names, names_size, s->name), s->addr);
            free(bytes);
            continue;
        }
        if (bytes) memcpy(dst, bytes, size);
        else memset(dst, 0, mem_size);
        free(bytes);
        for (u32 k = 0; k < img->section_count; k++)
            if (img->sections[k].address == s->addr && !strcmp(img->sections[k].name, str_at(names, names_size, s->name)))
                img->sections[k].size = mem_size;

        u32 end = s->addr + mem_size;
        if (s->addr >= WIIU_DATA_BASE && s->addr < WIIU_HEAP_BASE && end > img->data_end)
            img->data_end = end;
    }

    // 2. Walk the symbol table: imports, exports and small-data bases.
    u32 dimport_index = 0;   // numbering must match tools/rpx_patch.c
    for (u16 i = 0; i < shnum; i++) {
        if (sh[i].type != SHT_SYMTAB) continue;
        u32 sym_size = 0, str_size = 0;
        u8* syms = section_bytes(file, fsize, &sh[i], &sym_size);
        u8* strs = sh[i].link < shnum ? section_bytes(file, fsize, &sh[sh[i].link], &str_size) : NULL;
        for (u32 off = 0; syms && off + 16 <= sym_size; off += 16) {
            const char* name = str_at(strs, str_size, be32(syms + off));
            u32 value = be32(syms + off + 4);
            u16 shndx = be16(syms + off + 14);
            if (!strcmp(name, "_SDA_BASE_")) img->sda_base = value;
            if (!strcmp(name, "_SDA2_BASE_")) img->sda2_base = value;
            if (!strcmp(name, "__preinit_user")) img->preinit_user = value;
            u8 sym_type = syms[off + 12] & 0xF;
            if (name[0] && value && sym_type != 3 /* STT_SECTION */) {
                if (img->symbol_count % 1024 == 0)
                    img->symbols = (RpxSymbol*)realloc(img->symbols, (img->symbol_count + 1024) * sizeof(RpxSymbol));
                RpxSymbol* rs = &img->symbols[img->symbol_count++];
                rs->address = value;
                snprintf(rs->name, sizeof(rs->name), "%s", name);
            }
            if (shndx == 0 || shndx >= shnum || !name[0] || sym_type == 3) continue;

            const char* secname = str_at(names, names_size, sh[shndx].name);
            bool fimport = !strncmp(secname, ".fimport_", 9);
            bool dimport = !strncmp(secname, ".dimport_", 9);
            if (!fimport && !dimport) continue;
            if (img->import_count >= RPX_MAX_IMPORTS) continue;

            RpxImport* imp = &img->imports[img->import_count++];
            imp->address = value;
            imp->is_data = dimport;
            imp->data_object = dimport ? dimport_object_address(dimport_index++) : 0;
            snprintf(imp->library, sizeof(imp->library), "%s", secname + 9);
            snprintf(imp->name, sizeof(imp->name), "%s", name);
        }
        free(syms);
        free(strs);
    }

    // 2b. Relocations that point at imports.
    //
    // A PowerPC branch can only reach +/-32 MB, so game code at 0x02xxxxxx
    // can't branch straight to an import at 0xC0xxxxxx. Nintendo's tools aim
    // each such call at a "trampoline" slot just past the code, and the
    // console's loader fills that slot with a jump to the real function.
    // DolRecomp translated the branch as-is, so the recompiled code jumps to
    // the trampoline address; we record which import each one stands for.
    u32 tramp_count = 0, addr32_count = 0, addr16_count = 0;
    u32 data_ok = 0, data_filled = 0, data_other = 0;
    for (u16 r = 0; r < shnum; r++) {
        if (sh[r].type != SHT_RELA || sh[r].link >= shnum || sh[r].info >= shnum) continue;
        const Shdr* target = &sh[sh[r].info];
        const Shdr* symtab = &sh[sh[r].link];
        u32 rel_size = 0, sym_size = 0, str_size = 0;
        u8* rels = section_bytes(file, fsize, &sh[r], &rel_size);
        u8* syms = section_bytes(file, fsize, symtab, &sym_size);
        u8* strs = symtab->link < shnum ? section_bytes(file, fsize, &sh[symtab->link], &str_size) : NULL;

        for (u32 off = 0; rels && syms && off + 12 <= rel_size; off += 12) {
            u32 where = be32(rels + off);
            u32 info = be32(rels + off + 4);
            s32 addend = (s32)be32(rels + off + 8);
            u32 type = info & 0xFF, symidx = info >> 8;
            if ((u64)symidx * 16 + 16 > sym_size) continue;
            const u8* sym = syms + symidx * 16u;
            u16 shndx = be16(sym + 14);
            if (shndx == 0 || shndx >= shnum) continue;
            const char* secname = str_at(names, names_size, sh[shndx].name);
            bool fimport = !strncmp(secname, ".fimport_", 9);
            bool dimport = !strncmp(secname, ".dimport_", 9);
            const char* symname = str_at(strs, str_size, be32(sym));
            u32 symval = be32(sym + 4);

            // Pointers stored in data (not to imports): the console's loader
            // writes symbol + addend there at load. Most files already hold
            // the right value; fill in any that were left empty.
            if (!fimport && !dimport && type == R_PPC_ADDR32 && !(target->flags & 0x4u)) {
                u32 want = symval + (u32)addend;
                u32 have = wiiu_read32(where);
                if (have == want) data_ok++;
                else if (have == 0) { wiiu_write32(where, want); data_filled++; }
                else data_other++;
                for (int w = 0; w < g_debug_count; w++)
                    if (g_debug_addr[w] >= where && g_debug_addr[w] < where + 4)
                        printf("[rpx] relocation at %08X: pointer to %s+0x%X = %08X (file had %08X)\n",
                               where, symname[0] ? symname : secname, (u32)addend, want, have);
                continue;
            }
            if (!fimport && !dimport) continue;

            if (type == R_PPC_REL24 && (target->flags & 0x4u)) {
                // Where does the branch in the file actually go?
                u32 ins = wiiu_read32(where);
                s32 disp = (s32)(ins & 0x03FFFFFCu);
                if (disp & 0x02000000) disp -= 0x04000000;
                u32 dest = (ins & 2u) ? (u32)disp : where + (u32)disp;
                if (dest == symval || rpx_find_import(img, dest)) continue;   // already known
                if (img->import_count >= RPX_MAX_IMPORTS) continue;
                RpxImport* imp = &img->imports[img->import_count++];
                imp->address = dest;
                imp->is_data = false;
                snprintf(imp->library, sizeof(imp->library), "%s", secname + 9);
                snprintf(imp->name, sizeof(imp->name), "%s", symname);
                tramp_count++;
            } else if (type == R_PPC_ADDR32 && !(target->flags & 0x4u)) {
                // A pointer in data (e.g. a table of callbacks) to an import.
                const RpxImport* imp = rpx_find_import(img, symval);
                u32 base = (imp && imp->is_data) ? imp->data_object : symval;
                wiiu_write32(where, base + (u32)addend);
                addr32_count++;
            } else if (type == R_PPC_ADDR16_LO || type == R_PPC_ADDR16_HI || type == R_PPC_ADDR16_HA) {
                addr16_count++;
            }
        }
        free(rels);
        free(syms);
        free(strs);
    }
    printf("[rpx] import relocations: %u trampolines, %u data pointers, %u lis/addi pairs\n",
           tramp_count, addr32_count, addr16_count);
    printf("[rpx] data pointers: %u already set, %u filled in at load, %u differ (left alone)\n",
           data_ok, data_filled, data_other);

    // 3. RPL file info: Nintendo's tools store the small-data bases here
    //    (homebrew leaves them 0 and uses _SDA_BASE_ symbols instead).
    for (u16 i = 0; i < shnum; i++) {
        if (sh[i].type != SHT_RPL_FILEINFO) continue;
        u32 info_size = 0;
        u8* info = section_bytes(file, fsize, &sh[i], &info_size);
        if (info && info_size >= 0x2C) {
            u32 sda = be32(info + 0x24), sda2 = be32(info + 0x28);
            if (!img->sda_base && sda) img->sda_base = sda;
            if (!img->sda2_base && sda2) img->sda2_base = sda2;
        }
        free(info);
    }

    if (img->symbols) qsort(img->symbols, img->symbol_count, sizeof(RpxSymbol), compare_symbol);

    // 4. Remember where imports live so the per-jump check is cheap.
    img->import_min = 0xFFFFFFFFu;
    img->import_max = 0;
    for (u32 i = 0; i < img->import_count; i++) {
        if (img->imports[i].address < img->import_min) img->import_min = img->imports[i].address;
        if (img->imports[i].address > img->import_max) img->import_max = img->imports[i].address;
    }

    free(names);
    free(sh);
    free(file);

    if (!img->data_end) img->data_end = WIIU_DATA_BASE;
    return true;
}

static int compare_symbol(const void* a, const void* b) {
    u32 x = ((const RpxSymbol*)a)->address, y = ((const RpxSymbol*)b)->address;
    return x < y ? -1 : x > y;
}

void rpx_print_sections(const RpxImage* img, FILE* out) {
    fprintf(out, "[rpx] sections:\n");
    for (u32 i = 0; i < img->section_count; i++) {
        const RpxSection* s = &img->sections[i];
        if (!s->address) continue;
        fprintf(out, "  %-24s type=%08X flags=%08X %08X-%08X\n",
                s->name, s->type, s->flags, s->address, s->address + s->size);
    }
}

void rpx_describe_address(const RpxImage* img, u32 addr, FILE* out) {
    const RpxSection* in = NULL;
    for (u32 i = 0; i < img->section_count; i++) {
        const RpxSection* s = &img->sections[i];
        if (s->address && addr >= s->address && addr < s->address + s->size) in = s;
    }
    if (in) fprintf(out, "  0x%08X is in section %s (+0x%X)\n", addr, in->name, addr - in->address);
    else fprintf(out, "  0x%08X is not inside any section of the .rpx\n", addr);

    // Nearest named symbols on either side.
    u32 best_below = 0xFFFFFFFFu, best_above = 0xFFFFFFFFu;
    for (u32 i = 0; i < img->symbol_count; i++) {
        u32 a = img->symbols[i].address;
        if (a <= addr && (best_below == 0xFFFFFFFFu || a > img->symbols[best_below].address)) best_below = i;
        if (a > addr && (best_above == 0xFFFFFFFFu || a < img->symbols[best_above].address)) best_above = i;
    }
    if (best_below != 0xFFFFFFFFu)
        fprintf(out, "  nearest symbol before: %s at 0x%08X\n", img->symbols[best_below].name, img->symbols[best_below].address);
    if (best_above != 0xFFFFFFFFu)
        fprintf(out, "  nearest symbol after:  %s at 0x%08X\n", img->symbols[best_above].name, img->symbols[best_above].address);
}

const RpxImport* rpx_find_import(const RpxImage* img, u32 address) {
    for (u32 i = 0; i < img->import_count; i++)
        if (img->imports[i].address == address) return &img->imports[i];
    return NULL;
}
