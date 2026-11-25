// The NSR installer / launcher window, compiled into the game program.
// The source stays in launcher\nsr.c (with its theme next to it); this file
// only brings it into the runtime build. Entry: nsr_wizard() in main.c.
#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "../../launcher/nsr.c"
#endif
