// Compiling GLSL to SPIR-V (the shader format Vulkan takes).
#ifndef GLSL_COMPILE_H
#define GLSL_COMPILE_H

#include "cpu/cpu.h"
#include <stdbool.h>
#include <stddef.h>

// Returns malloc'd SPIR-V words (free with free()), or NULL with the error
// printed.
u32* glsl_to_spirv(bool fragment, const char* src, size_t* words);

// Set glslang up (once, before compiling on several threads).
void glsl_init(void);

#endif
