// Accounts and online services, answered like an offline console (online.c).
#ifndef ONLINE_H
#define ONLINE_H

#include "hle.h"

const HleEntry* online_hle_table(size_t* count);
const HleEntry* online_net_hle_table(size_t* count);   // (net_offline.c: the network calls, answered offline)
const HleEntry* boss_hle_table(size_t* count);         // (net_offline.c: the BOSS download calls, answered offline)
bool splat_online_on(void);

#endif
