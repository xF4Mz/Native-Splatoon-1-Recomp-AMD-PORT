// Assorted system calls (calendar, locked cache, ProcUI, settings, accounts).
#ifndef SYS_H
#define SYS_H

#include "hle.h"

const HleEntry* sys_hle_table(size_t* count);

// Write the account's default Mii (FFLStoreData, 0x60 bytes) to guest memory.
void sys_write_default_mii(u32 out);

#endif
