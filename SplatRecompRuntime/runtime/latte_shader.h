// Translating the Wii U GPU's shader programs (AMD R700-family "Latte"
// machine code, as the game hands it to GX2) into GLSL, which glslang then
// turns into SPIR-V for Vulkan. This is a straight translation of each
// instruction; no emulation of the GPU happens at run time.
//
// Data a shader reads comes from one storage buffer the renderer fills per
// draw (uniform blocks, uniform registers, vertex data) plus textures. The
// buffer starts with a small table (HDR_*) saying where everything is.
#ifndef LATTE_SHADER_H
#define LATTE_SHADER_H

#include "cpu/cpu.h"
#include <stdbool.h>

#define LS_TEX_UNITS 18
#define LS_VS_TEX_UNITS 18       // vertex shader texture units we bind
#define LS_BINDING_VS_TEX 1      // bindings 1..18
#define LS_BINDING_PS_TEX 20     // bindings 20..37
#define LS_BINDING_STREAMOUT 40  // writable buffer for stream-out (transform feedback)

// Draw table layout (in 32-bit words, at the start of each draw's data).
enum {
    HDR_ATTR_BASE = 0,       // 16: byte offset of attribute buffer data, minus where the upload starts
    HDR_ATTR_STRIDE = 16,    // 16: bytes between vertices
    HDR_STEP_RATE = 32,      // 2: instance step rates
    HDR_VS_UB = 34,          // 16: word offset of each vertex shader uniform block
    HDR_PS_UB = 50,          // 16: same for the pixel shader
    HDR_VS_CFILE = 66,       // word offset of the vertex shader's uniform registers
    HDR_PS_CFILE = 67,
    HDR_ALPHA_REF = 68,      // float
    HDR_WIN_SCALE = 69,      // 2 floats: window position -> clip space (special state)
    HDR_POINT_SIZE = 71,
    HDR_VS_UB_SIZE = 72,     // 16: size of each block in vec4s
    HDR_PS_UB_SIZE = 88,     // 16
    HDR_SO_BASE = 104,       // 4: word offset of each stream-out buffer's data for this draw
    HDR_SO_STRIDE = 108,     // 4: words per vertex
    HDR_SO_FIRST = 112,      // first vertex index of the draw
    HDR_SO_VPI = 113,        // vertices per instance
    HDR_SO_ON = 114,         // stream-out active for this draw
    HDR_WORDS = 128,
};

// Texture unit description (part of the shader key).
typedef struct {
    u8 dim;    // 0 none/unbound, else resource DIM + 1 (1 = 1D, 2 = 2D, 3 = 3D, 4 = cube, 5 = 1D array, 6 = 2D array, 7 = MSAA, 8 = MSAA array)
    u8 kind;   // 0 float, 1 signed int, 2 unsigned int
} LatteTexKey;

typedef struct {
    // vertex shader
    const u8* vs; u32 vs_size;
    const u8* fs; u32 fs_size;          // fetch shader
    u8 vtx_semantic[32];                 // SQ_VTX_SEMANTIC_n & 0xFF (0xFF none)
    u8 vs_out_semantic[40];              // semantic of each parameter export (SPI_VS_OUT_ID)
    u32 vte_cntl, clip_cntl;
    bool points;
    LatteTexKey vs_tex[LS_TEX_UNITS];
    // pixel shader
    const u8* ps; u32 ps_size;
    u32 ps_input_cntl[32];
    u32 ps_in_control0, ps_in_control1;
    u32 cb_shader_mask;
    u32 alpha_test;                      // SX_ALPHA_TEST_CONTROL
    u8 cb_kind[8];                       // color target number type: 0 float, 1 sint, 2 uint
    LatteTexKey ps_tex[LS_TEX_UNITS];
} LatteShaderKey;

typedef struct {
    char* glsl;                // malloc'd; free with free()
    // what the shader reads (for the renderer's uploads)
    u32 attrib_used;           // attribute buffers read per vertex
    u32 attrib_instanced;      // attribute buffers read per instance (pixel shaders: LS_PS_CUBE_ARRAYS)
    u32 attrib_extent[16];     // bytes past a vertex's start that are read
    u32 ub_used;               // uniform blocks
    u32 cfile_count;           // uniform registers used (vec4s), 0 = none
    u32 tex_used;              // texture units
    u32 tex_shadow;            // units read with depth compare (shadow maps)
    u32 so_mask;               // stream-out buffers the vertex shader writes
    // pixel shader
    u32 color_outputs;         // color targets written
    bool writes_depth;
    int warnings;
} LatteShaderOut;

// Pixel shaders: texture units read as cube-map arrays (the shader picks the
// cube with SET_CUBEMAP_INDEX). Kept in attrib_instanced, which pixel shaders
// don't otherwise use, so the disk cache's records keep their layout.
#define LS_PS_CUBE_ARRAYS(o) ((o)->attrib_instanced)

// Returns false (with a message printed) if the program can't be translated.
bool latte_decompile_vs(const LatteShaderKey* key, LatteShaderOut* out);
bool latte_decompile_ps(const LatteShaderKey* key, LatteShaderOut* out);
u32 latte_shader_changed_since_cache(void);   // 1: abs/neg operands (since cache 0x53430007), 2: cube-map array index (v0.19)
u32 latte_shader_tex_mask(const u8* prog, u32 size);
u64 latte_shader_param_exports(const u8* prog, u32 size);   // bit n: the program writes parameter export n (0..39)

#endif
