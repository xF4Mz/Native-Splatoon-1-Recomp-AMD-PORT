// rpx_patch (tools\rpx_patch.c) as a function inside the game program:
// rpx_patch_main(argc, argv), printing to the launcher window (wiz_printf).
// The installer runs it on the player's update's code\Gambit.rpx to make
// data\Gambit_v272_patched.rpx in the install folder.
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
#define main rpx_patch_main
#include "../tools/rpx_patch.c"
#undef main
#endif
