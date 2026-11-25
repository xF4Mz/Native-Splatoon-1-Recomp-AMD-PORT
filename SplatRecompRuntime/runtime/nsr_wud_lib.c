// The disc image reader (launcher\nsr_wud.c) as a function inside the game
// program: nsr_wud_main(argc, argv) with its printf lines going to the
// launcher window (wiz_printf in nsr.c). The standalone nsr_wud.exe is the
// same source built on its own (launcher\build_wud.cmd).
#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdarg.h>
int wiz_printf(const char* fmt, ...);
int wiz_fprintf(FILE* f, const char* fmt, ...);
#define printf wiz_printf
#define fprintf wiz_fprintf
#define main nsr_wud_main
#include "../../launcher/nsr_wud.c"
#undef main
#endif
