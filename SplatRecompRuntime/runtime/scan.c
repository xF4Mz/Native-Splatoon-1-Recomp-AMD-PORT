// Code scanner for debugging: finds the instructions in the game's code
// that store to (or load from) a given address, and who calls them.
//
// PowerPC code reaches a global with "lis rX, high" (maybe "addi rX, rX,
// low") and then "stw rY, offset(rX)", or through r13/r2 (small data). We
// follow each store's base register back a few instructions to work out
// the address it touches.
#include "scan.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <string.h>

static int32_t sext16(u32 v) { return (int16_t)(v & 0xFFFF); }

static bool is_store(u32 ins) {
    u32 op = ins >> 26;
    return op == 36 || op == 37 || op == 38 || op == 39 || op == 44 || op == 45 ||
           op == 47 || (op >= 52 && op <= 55);
}
static bool is_load(u32 ins) {
    u32 op = ins >> 26;
    return op == 32 || op == 33 || op == 34 || op == 35 || op == 40 || op == 41 ||
           op == 42 || op == 43 || op == 46 || (op >= 48 && op <= 51);
}

// Value of register r just before address `at`, if it was built by
// lis/addi/ori nearby. Returns false if unknown.
static bool reg_value(const RpxImage* img, u32 start, u32 at, int r, u32* out, int depth) {
    if (r == 13) { *out = img->sda_base; return true; }
    if (r == 2) { *out = img->sda2_base; return true; }
    if (depth > 3) return false;
    for (u32 a = at - 4, n = 0; a >= start && n < 160; a -= 4, n++) {
        u32 ins = wiiu_read32(a);
        u32 op = ins >> 26;
        int rd = (int)((ins >> 21) & 31), ra = (int)((ins >> 16) & 31);
        if (op == 18 || op == 16 || op == 19) {
            // A call wipes the "volatile" registers (r0, r3-r12); r14-r31
            // survive calls, so keep tracing those.
            if ((ins & 1) && (r == 0 || (r >= 3 && r <= 12))) return false;
            continue;
        }
        // Update forms (lwzu, stwu, lbzu...): besides loading/storing they
        // set rA to the address they used, rA + d.
        bool update_form = op == 33 || op == 35 || op == 41 || op == 43 || op == 49 || op == 51 ||
                           op == 37 || op == 39 || op == 45 || op == 53 || op == 55;
        if (update_form && ra == r) {
            u32 base = 0;
            if (!reg_value(img, start, a, ra, &base, depth + 1)) return false;
            *out = base + (u32)sext16(ins);
            return true;
        }
        if (is_store(ins) || op == 11 || op == 10) continue;   // stores, compares
        if (op == 31 && ((ins >> 1) & 0x3FF) == 444) {      // or rA,rS,rB (mr when rS == rB)
            int rs = (int)((ins >> 21) & 31), rb = (int)((ins >> 11) & 31);
            if (ra != r) continue;
            if (rs == rb) return reg_value(img, start, a, rs, out, depth + 1);
            return false;
        }
        if ((op == 24 || op == 25) && ra == r) {   // ori / oris rA, rS, imm
            u32 base = 0;
            if (!reg_value(img, start, a, rd, &base, depth + 1)) return false;
            *out = op == 24 ? (base | (ins & 0xFFFF)) : (base | ((ins & 0xFFFF) << 16));
            return true;
        }
        if (op == 31 || op == 24 || op == 25 || op == 26 || op == 27 || op == 28 || op == 29 || op == 21) {
            if (ra == r) return false;   // logic ops write rA
            continue;
        }
        if (rd != r) continue;
        if (op == 15) {                                 // addis rd, ra, imm  (lis when ra=0)
            u32 base = 0;
            if (ra && !reg_value(img, start, a, ra, &base, depth + 1)) return false;
            *out = base + ((ins & 0xFFFF) << 16);
            return true;
        }
        if (op == 14) {                                 // addi rd, ra, imm  (li when ra=0)
            u32 base = 0;
            if (ra && !reg_value(img, start, a, ra, &base, depth + 1)) return false;
            *out = base + (u32)sext16(ins);
            return true;
        }
        if (op == 24 && ra == r) { /* ori: rs is bits 21-25, ra is dest */ }
        return false;   // something else set it
    }
    return false;
}

// Start of the function containing `at`: just after the previous function's
// final blr, or its own stack-frame setup (stwu r1,-N(r1)), whichever is
// closer.
u32 scan_function_start(u32 at, u32 sec_start) {
    u32 a = at;
    for (u32 n = 0; a > sec_start && n < 8192; a -= 4, n++) {
        u32 prev = wiiu_read32(a - 4);
        if (prev == 0x4E800020u) return a;                         // blr
        if ((wiiu_read32(a) & 0xFFFF8000u) == 0x94218000u)            // stwu r1,-N(r1)
            return prev == 0x7C0802A6u ? a - 4 : a;                    // (mflr r0 may come first)
    }
    return a;
}

static void list_callers(const RpxImage* img, u32 target) {
    int found = 0;
    for (u32 s = 0; s < img->section_count; s++) {
        const RpxSection* sec = &img->sections[s];
        if (!(sec->flags & 4) || !sec->size) continue;
        for (u32 a = sec->address; a < sec->address + sec->size; a += 4) {
            u32 ins = wiiu_read32(a);
            if ((ins >> 26) != 18 || (ins & 3) != 1) continue;   // bl
            int32_t disp = (int32_t)(ins & 0x03FFFFFCu);
            if (disp & 0x02000000) disp -= 0x04000000;
            if (a + (u32)disp != target) continue;
            if (found++ < 12) printf("      called from %08X (function at %08X)\n", a, scan_function_start(a, sec->address));
        }
    }
    if (!found) printf("      (no direct calls found: probably called through a pointer/vtable)\n");
    else if (found > 12) printf("      ... and %d more\n", found - 12);
}

void scan_for_address(const RpxImage* img, u32 target) {
    printf("[scan] looking for code that touches %08X (value there at load: %08X)\n", target, wiiu_read32(target));
    int stores = 0, loads = 0;
    for (u32 s = 0; s < img->section_count; s++) {
        const RpxSection* sec = &img->sections[s];
        if (!(sec->flags & 4) || !sec->size) continue;
        for (u32 a = sec->address; a < sec->address + sec->size; a += 4) {
            u32 ins = wiiu_read32(a);
            bool st = is_store(ins), ld = is_load(ins);
            if ((ins >> 26) == 14 && ((ins >> 16) & 31)) {   // addi: address taken?
                u32 base;
                u32 taken;
                if (reg_value(img, sec->address, a, (int)((ins >> 16) & 31), &base, 0) &&
                    (taken = base + (u32)sext16(ins)) + 0x40 > target && taken <= target) {
                    u32 fn = scan_function_start(a, sec->address);
                    printf("  ADDRESS TAKEN at %08X: %08X (target is +0x%X from it), function at %08X\n",
                           a, taken, target - taken, fn);
                    u32 end = a + 64;
                    for (u32 p = (end - fn > 0x400 ? end - 0x400 : fn), k = 0; p < end; p += 4, k++)
                        printf("%s%08X", (k % 8) ? " " : (k ? "\n      " : "      "), wiiu_read32(p));
                    printf("\n");
                    list_callers(img, fn);
                }
                continue;
            }
            if (!st && !ld) continue;
            int ra = (int)((ins >> 16) & 31);
            if (!ra) continue;
            u32 base;
            if (!reg_value(img, sec->address, a, ra, &base, 0)) continue;
            u32 ea = base + (u32)sext16(ins);
            u32 span = (ins >> 26) == 47 ? 4 * (32 - ((ins >> 21) & 31)) : 4;
            if (ea > target || ea + span <= target) continue;
            if (ld) {
                if (loads++ < 8) printf("  read  at %08X (function at %08X)\n", a, scan_function_start(a, sec->address));
                continue;
            }
            stores++;
            u32 fn = scan_function_start(a, sec->address);
            printf("  WRITE at %08X (instruction %08X), in the function at %08X\n", a, ins, fn);
            printf("    code from the function start to just after the write:\n");
            u32 end = a + 12;
            if (end - fn > 0x400) fn = end - 0x400;
            for (u32 p = fn, k = 0; p < end; p += 4, k++)
                printf("%s%08X", (k % 8) ? " " : (k ? "\n      " : "      "), wiiu_read32(p));
            printf("\n");
            list_callers(img, scan_function_start(a, sec->address));
        }
    }
    // Loose pass: any store whose offset matches the low half of the address,
    // even when the base register can't be traced. Catches unusual patterns.
    int loose = 0;
    u16 lo = (u16)(target & 0xFFFF);
    for (u32 s = 0; s < img->section_count; s++) {
        const RpxSection* sec = &img->sections[s];
        if (!(sec->flags & 4) || !sec->size) continue;
        for (u32 a = sec->address; a < sec->address + sec->size; a += 4) {
            u32 ins = wiiu_read32(a);
            if (!is_store(ins)) continue;
            bool hit = (u16)(ins & 0xFFFF) == lo;
            if ((ins >> 26) == 47) {   // stmw rS, d(rA): stores rS..r31 from d upward
                u32 d = (u32)sext16(ins), n = 32 - ((ins >> 21) & 31);
                u32 l = (u32)(int16_t)lo;
                hit = l >= d && l < d + 4 * n && ((l - d) & 3) == 0;
            }
            if (!hit) continue;
            int ra = (int)((ins >> 16) & 31);
            u32 base;
            bool known = reg_value(img, sec->address, a, ra, &base, 0);
            if (known) continue;   // already judged above
            u32 fn = scan_function_start(a, sec->address);
            if (loose++ < 12) {
                printf("  maybe: store at %08X (instruction %08X, base register r%d unknown), function at %08X\n",
                       a, ins, ra, fn);
                u32 from = a >= fn + 0x60 ? a - 0x60 : fn;
                for (u32 p = from, k = 0; p < a + 8; p += 4, k++)
                    printf("%s%08X", (k % 8) ? " " : (k ? "\n      " : "      "), wiiu_read32(p));
                printf("\n");
                list_callers(img, fn);
            }
        }
    }
    if (loose > 12) printf("  ... %d possible stores in total\n", loose);

    // Broad pass: every addi/ori that builds an address within 0x40 below
    // the target (a pointer to it, or to a small struct containing it).
    int built = 0;
    for (u32 s = 0; s < img->section_count; s++) {
        const RpxSection* sec = &img->sections[s];
        if (!(sec->flags & 4) || !sec->size) continue;
        for (u32 a = sec->address; a < sec->address + sec->size; a += 4) {
            u32 ins = wiiu_read32(a);
            u32 op = ins >> 26;
            if (op != 14 && op != 24) continue;
            u32 imm = op == 14 ? (u32)sext16(ins) : (ins & 0xFFFF);
            u32 lo_t = target & 0xFFFF;
            if (((lo_t - imm) & 0xFFFF) > 0x40) continue;   // quick filter on the low half
            int src = op == 14 ? (int)((ins >> 16) & 31) : (int)((ins >> 21) & 31);
            if (op == 14 && !src) continue;
            u32 base;
            if (!reg_value(img, sec->address, a, src, &base, 0)) continue;
            u32 v = op == 14 ? base + imm : (base | imm);
            if (v > target || target - v > 0x40) continue;
            u32 fn = scan_function_start(a, sec->address);
            if (built++ < 16) {
                printf("  BUILDS %08X (target +0x%X) at %08X with %s, function at %08X\n",
                       v, target - v, a, op == 14 ? "addi" : "ori", fn);
                u32 from = a >= fn + 0x40 ? a - 0x40 : fn;
                for (u32 p = from, k = 0; p < a + 0x40; p += 4, k++)
                    printf("%s%08X", (k % 8) ? " " : (k ? "\n      " : "      "), wiiu_read32(p));
                printf("\n");
                list_callers(img, fn);
            }
        }
    }
    printf("[scan] %d places build an address at or just below the target\n", built);
    if (loads > 8) printf("  ... %d reads in total\n", loads);
    printf("[scan] %d writes, %d reads found\n", stores, loads);
}

// --- Call tree ---------------------------------------------------------------
static int g_tree_lines;

static void callers_rec(const RpxImage* img, u32 target, int depth, u32* seen, int* seen_n) {
    if (depth > 5 || g_tree_lines > 80) return;
    for (int i = 0; i < *seen_n; i++) if (seen[i] == target) return;
    if (*seen_n < 512) seen[(*seen_n)++] = target;
    int found = 0;
    for (u32 s = 0; s < img->section_count; s++) {
        const RpxSection* sec = &img->sections[s];
        if (!sec->size || sec->address < 0x02000000u || sec->address >= 0xC0000000u) continue;
        bool code = (sec->flags & 4) != 0;
        for (u32 a = sec->address; a + 4 <= sec->address + sec->size; a += 4) {
            u32 w = wiiu_read32(a);
            if (code) {
                u32 dest;
                if ((w >> 26) == 18) {                         // b / bl
                    int32_t disp = (int32_t)(w & 0x03FFFFFCu);
                    if (disp & 0x02000000) disp -= 0x04000000;
                    dest = (w & 2) ? (u32)disp : a + (u32)disp;
                } else if ((w >> 26) == 16) {                  // bc / bcl (conditional)
                    int32_t disp = (int16_t)(w & 0xFFFC);
                    dest = (w & 2) ? (u32)disp : a + (u32)disp;
                } else continue;
                if (dest != target) continue;
                u32 fn = scan_function_start(a, sec->address);
                if (fn == target) continue;                      // a loop inside the function itself
                found++;
                if (g_tree_lines++ > 80) return;
                printf("  %*s<- %s at %08X, from function %08X\n", depth * 2, "",
                       (w & 1) ? "called" : "jumped to (tail call)", a, fn);
                callers_rec(img, fn, depth + 1, seen, seen_n);
            } else if (w == target) {
                found++;
                if (g_tree_lines++ > 80) return;
                printf("  %*s<- its address is stored in data at %08X (a table of functions, e.g. a vtable)\n",
                       depth * 2, "", a);
            }
        }
    }
    // Code that builds the function's address in a register (lis + addi/ori),
    // to call it through a pointer later.
    for (u32 s = 0; s < img->section_count; s++) {
        const RpxSection* sec = &img->sections[s];
        if (!(sec->flags & 4) || !sec->size) continue;
        for (u32 a = sec->address; a < sec->address + sec->size; a += 4) {
            u32 ins = wiiu_read32(a);
            u32 op = ins >> 26;
            if (op != 14 && op != 24) continue;
            u32 imm = op == 14 ? (u32)sext16(ins) : (ins & 0xFFFF);
            if ((u16)imm != (u16)target) continue;
            int src = op == 14 ? (int)((ins >> 16) & 31) : (int)((ins >> 21) & 31);
            if (op == 14 && !src) continue;
            u32 base;
            if (!reg_value(img, sec->address, a, src, &base, 0)) continue;
            if ((op == 14 ? base + imm : (base | imm)) != target) continue;
            u32 fn = scan_function_start(a, sec->address);
            found++;
            if (g_tree_lines++ > 80) return;
            printf("  %*s<- address built at %08X (called through a pointer), in function %08X\n",
                   depth * 2, "", a, fn);
            callers_rec(img, fn, depth + 1, seen, seen_n);
        }
    }
    if (!found) {
        g_tree_lines++;
        printf("  %*s(nothing calls %08X directly)\n", depth * 2, "", target);
    }
}

void scan_callers(const RpxImage* img, u32 function) {
    static u32 seen[512];
    int seen_n = 0;
    g_tree_lines = 0;
    printf("[callers] who calls %08X (indented = one level further up)\n", function);
    callers_rec(img, function, 0, seen, &seen_n);
}

// Finds a piece of text (e.g. part of a message the game prints) in the
// game's data, then the code that uses it.
void scan_find_text(const RpxImage* img, const char* text) {
    size_t n = strlen(text);
    int found = 0;
    printf("[scan] looking for the text \"%s\"\n", text);
    for (u32 s = 0; s < img->section_count && found < 4; s++) {
        const RpxSection* sec = &img->sections[s];
        if ((sec->flags & 4) || !sec->size || sec->address < 0x10000000u || sec->address >= 0xC0000000u) continue;
        const u8* p = wiiu_mem_ptr(sec->address, sec->size);
        if (!p) continue;
        for (u32 i = 0; i + n <= sec->size && found < 4; i++) {
            if (memcmp(p + i, text, n) != 0) continue;
            u32 at = sec->address + i;
            u32 start = at;   // back up to the start of the whole string
            while (start > sec->address && p[start - 1 - sec->address] >= 0x20 && p[start - 1 - sec->address] < 0x7F) start--;
            printf("[scan] found it at %08X (string starts at %08X)\n", at, start);
            scan_for_address(img, start);
            found++;
            i += (u32)n;
        }
    }
    if (!found) printf("[scan] not found\n");
}
