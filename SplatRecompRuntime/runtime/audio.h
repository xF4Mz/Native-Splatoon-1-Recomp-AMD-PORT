// The Wii U sound library (AX): keeps the game's sound engine ticking (see audio.c).
#ifndef AUDIO_H
#define AUDIO_H

#include "hle.h"

const HleEntry* audio_hle_table(size_t* count);
#include <stdio.h>
void audio_report_voices(FILE* out);   // (the freeze watchdog)

#endif
