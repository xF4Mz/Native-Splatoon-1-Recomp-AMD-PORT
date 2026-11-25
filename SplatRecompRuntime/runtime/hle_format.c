// printf-style formatting for guest calls like OSReport.
//
// On PowerPC, a variadic call puts integer/pointer arguments in r3..r10
// (then on the stack) and floating-point ones in f1..f8. We walk the format
// string and pull each argument from the right place.
#include "hle.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    CPUState* c;
    int gpr;       // next integer register (3..10)
    int fpr;       // next float register (1..8)
    u32 stack;     // next stack slot once registers run out
} VaState;

static u32 next_u32(VaState* va) {
    if (va->gpr <= 10) return va->c->gpr[va->gpr++];
    u32 v = wiiu_read32(va->stack);
    va->stack += 4;
    return v;
}

static u64 next_u64(VaState* va) {
    // 64-bit values start in an odd register (r3, r5, r7, r9).
    if (va->gpr <= 10 && (va->gpr & 1) == 0) va->gpr++;
    if (va->gpr <= 9) {
        u64 hi = va->c->gpr[va->gpr], lo = va->c->gpr[va->gpr + 1];
        va->gpr += 2;
        return (hi << 32) | lo;
    }
    va->gpr = 11;
    va->stack = (va->stack + 7) & ~7u;
    u64 hi = wiiu_read32(va->stack), lo = wiiu_read32(va->stack + 4);
    va->stack += 8;
    return (hi << 32) | lo;
}

static double next_double(VaState* va) {
    if (va->fpr <= 8) return va->c->fpr[va->fpr++];
    va->stack = (va->stack + 7) & ~7u;
    u64 bits = ((u64)wiiu_read32(va->stack) << 32) | wiiu_read32(va->stack + 4);
    va->stack += 8;
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

int hle_format(CPUState* c, int fmt_reg, char* out, size_t out_size) {
    char fmt[1024];
    wiiu_read_string(c->gpr[fmt_reg], fmt, sizeof(fmt));

    VaState va = { c, fmt_reg + 1, 1, c->gpr[1] + 8 };
    size_t pos = 0;
    out[0] = '\0';

    for (const char* p = fmt; *p && pos + 1 < out_size; ) {
        if (*p != '%') { out[pos++] = *p++; continue; }

        // Copy one conversion spec, e.g. "%-08.3lld".
        char spec[32];
        size_t n = 0;
        spec[n++] = *p++;
        while (*p && strchr("-+ #0", *p) && n < 20) spec[n++] = *p++;
        if (*p == '*') { n += (size_t)snprintf(spec + n, sizeof(spec) - n, "%d", (int)next_u32(&va)); p++; }
        else while (*p >= '0' && *p <= '9' && n < 24) spec[n++] = *p++;
        if (*p == '.') {
            spec[n++] = *p++;
            if (*p == '*') { n += (size_t)snprintf(spec + n, sizeof(spec) - n, "%d", (int)next_u32(&va)); p++; }
            else while (*p >= '0' && *p <= '9' && n < 28) spec[n++] = *p++;
        }
        int longness = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L') {
            if (*p == 'l' || *p == 'j' || *p == 'L') longness++;
            p++;
        }
        char conv = *p ? *p++ : '\0';
        char buf[512];
        buf[0] = '\0';

        switch (conv) {
        case 'd': case 'i': {
            spec[n++] = 'l'; spec[n++] = 'l'; spec[n++] = 'd'; spec[n] = '\0';
            long long v = longness >= 2 ? (long long)(s64)next_u64(&va) : (long long)(s32)next_u32(&va);
            snprintf(buf, sizeof(buf), spec, v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            spec[n++] = 'l'; spec[n++] = 'l'; spec[n++] = conv; spec[n] = '\0';
            unsigned long long v = longness >= 2 ? next_u64(&va) : next_u32(&va);
            snprintf(buf, sizeof(buf), spec, v);
            break;
        }
        case 'p':
            snprintf(buf, sizeof(buf), "0x%08X", next_u32(&va));
            break;
        case 'c':
            spec[n++] = 'c'; spec[n] = '\0';
            snprintf(buf, sizeof(buf), spec, (int)(char)next_u32(&va));
            break;
        case 's': {
            char str[400];
            u32 addr = next_u32(&va);
            if (addr) wiiu_read_string(addr, str, sizeof(str));
            else snprintf(str, sizeof(str), "(null)");
            spec[n++] = 's'; spec[n] = '\0';
            snprintf(buf, sizeof(buf), spec, str);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            spec[n++] = conv; spec[n] = '\0';
            snprintf(buf, sizeof(buf), spec, next_double(&va));
            break;
        case '%':
            snprintf(buf, sizeof(buf), "%%");
            break;
        default:
            snprintf(buf, sizeof(buf), "%%%c", conv);
            break;
        }

        for (const char* b = buf; *b && pos + 1 < out_size; b++) out[pos++] = *b;
    }

    out[pos] = '\0';
    return (int)pos;
}
