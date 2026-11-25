#ifndef CPU_FALLBACK_H
#define CPU_FALLBACK_H

#include "cpu/cpu.h"

// Handle instructions DolRecomp passes back to the runtime (cache
// maintenance, special registers).
void cpu_fallback_attach(CPUState* cpu);

#endif
