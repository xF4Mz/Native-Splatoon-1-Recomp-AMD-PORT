// Instructions DolRecomp hands back to the runtime.
//
// A few PowerPC instructions have no meaning on a PC (cache maintenance) or
// depend on the machine (special-purpose registers). DolRecomp's generated
// code calls cpu->instruction_fallback for those; we handle them here and
// move pc past the instruction.
#include "cpu_fallback.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <string.h>

static u32 g_reported[64];
static u32 g_reported_count = 0;

static void report_once(u32 raw, u32 cia, const char* what) {
    u32 key = raw & 0xFC0007FEu;   // opcode + extended opcode
    for (u32 i = 0; i < g_reported_count; i++)
        if (g_reported[i] == key) return;
    if (g_reported_count < 64) g_reported[g_reported_count++] = key;
    fprintf(stderr, "[cpu] %s: instruction %08X at %08X\n", what, raw, cia);
}

static void fallback(CPUState* cpu, u32 raw, u32 cia) {
    u32 op = raw >> 26;
    u32 xo = (raw >> 1) & 0x3FF;
    u32 rd = (raw >> 21) & 31;

    if (op == 31) {
        switch (xo) {
        case 54:    // dcbst: store cache block
        case 86:    // dcbf:  flush cache block
        case 470:   // dcbi:  invalidate cache block
        case 982:   // icbi:  invalidate instruction cache block
            cpu->pc = cia + 4;   // the PC keeps memory coherent for us
            return;

        case 339: { // mfspr
            u32 spr = ((raw >> 16) & 31) | (((raw >> 11) & 31) << 5);
            u32 value = 0;
            switch (spr) {
            case 287:  value = 0x70010201u; break;   // PVR: Espresso
            case 1007: value = 1; break;             // UPIR: we run as core 1
            default:   report_once(raw, cia, "read of an unhandled special register (returning 0)"); break;
            }
            cpu->gpr[rd] = value;
            cpu->pc = cia + 4;
            return;
        }
        case 467:   // mtspr: writes to machine registers we don't model
            report_once(raw, cia, "write to an unhandled special register (ignored)");
            cpu->pc = cia + 4;
            return;
        }
    }

    report_once(raw, cia, "unsupported instruction");
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
}

void cpu_fallback_attach(CPUState* cpu) {
    cpu->instruction_fallback = fallback;
}
