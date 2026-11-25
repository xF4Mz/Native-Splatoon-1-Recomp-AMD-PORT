// The Wii U's GPU ("Latte", an AMD R700-family chip): command packets and
// register numbers shared by the GX2 library (gx2.c, which writes commands
// the way the console's GX2 does) and the command processor (latte_cp.c,
// which reads them and drives the PC's graphics card).
//
// GX2 writes PM4 packets: a header word, then data words. Most packets set
// registers; the rest draw, copy or synchronise. Registers live in one flat
// space of 0x10000 32-bit words split into areas (config, context, shader
// constants, resources, samplers). Register numbers and bit layouts follow
// AMD's R6xx/R7xx documentation and Cemu's notes.
#ifndef LATTE_H
#define LATTE_H

#include <stdint.h>

// --- PM4 type-3 packet opcodes ------------------------------------------------
#define IT_NOP                   0x10
#define IT_SET_PREDICATION       0x20
#define IT_DRAW_INDEX_2          0x27
#define IT_CONTEXT_CONTROL       0x28
#define IT_INDEX_TYPE            0x2A
#define IT_DRAW_INDEX_AUTO       0x2D
#define IT_DRAW_INDEX_IMMD       0x2E
#define IT_NUM_INSTANCES         0x2F
#define IT_INDIRECT_BUFFER_PRIV  0x32
#define IT_STRMOUT_BUFFER_UPDATE 0x34
#define IT_MEM_SEMAPHORE         0x39
#define IT_WAIT_REG_MEM          0x3C
#define IT_MEM_WRITE             0x3D
#define IT_SURFACE_SYNC          0x43
#define IT_EVENT_WRITE           0x46
#define IT_EVENT_WRITE_EOP       0x47
#define IT_LOAD_CONFIG_REG       0x60
#define IT_LOAD_CONTEXT_REG      0x61
#define IT_LOAD_ALU_CONST        0x62
#define IT_LOAD_BOOL_CONST       0x63
#define IT_LOAD_LOOP_CONST       0x64
#define IT_LOAD_RESOURCE         0x65
#define IT_LOAD_SAMPLER          0x66
#define IT_SET_CONFIG_REG        0x68
#define IT_SET_CONTEXT_REG       0x69
#define IT_SET_ALU_CONST         0x6A
#define IT_SET_LOOP_CONST        0x6C
#define IT_SET_RESOURCE          0x6D
#define IT_SET_SAMPLER           0x6E
#define IT_SET_CTL_CONST         0x6F
#define IT_STRMOUT_BASE_UPDATE   0x72
#define IT_SET_ALL_CONTEXTS      0x74

// Our own packets (opcodes the real GPU doesn't use), for work the console's
// GX2 does with GPU programs we don't have: clears, copies, presenting.
#define IT_HLE_COPY_SURFACE      0xEE
#define IT_HLE_QUERY             0xED   // occlusion query: (1 begin / 2 end, query address)
#define IT_HLE_SWAP              0xF0   // present the TV scan buffer
#define IT_HLE_COPY_TO_SCAN      0xF3   // color buffer -> TV/GamePad scan buffer
#define IT_HLE_CLEAR             0xF5   // clear color and/or depth/stencil
#define IT_HLE_SAMPLE_TIMER      0xF7
#define IT_HLE_RT_INFO           0xFC   // logical size of a color/depth buffer
#define IT_HLE_FENCE             0xFD   // "GPU reached this point" (timestamps)

#define PM4_TYPE3(op, count) (0xC0000000u | ((uint32_t)(op) << 8) | ((uint32_t)((count) - 1) << 16))
#define PM4_FILLER 0x80000000u   // type-2 packet: padding

// --- Register areas -------------------------------------------------------------
#define REG_BASE_CONFIG    0x2000
#define REG_BASE_CONTEXT   0xA000
#define REG_BASE_ALU_CONST 0xC000
#define REG_BASE_RESOURCE  0xE000
#define REG_BASE_SAMPLER   0xF000
#define REG_BASE_CTL_CONST 0xF3FC
#define REG_BASE_LOOP_CONST 0xF880
#define REG_COUNT          0x10000

// --- Registers we use -----------------------------------------------------------
enum {
    VGT_PRIMITIVE_TYPE = 0x2256,
    SQ_CONFIG = 0x2300,
    TD_PS_SAMPLER0_BORDER = 0x2900,
    TD_VS_SAMPLER0_BORDER = 0x2980,
    TD_GS_SAMPLER0_BORDER = 0x2A00,

    DB_DEPTH_SIZE = 0xA000,
    DB_DEPTH_VIEW = 0xA001,
    DB_DEPTH_BASE = 0xA003,
    DB_DEPTH_INFO = 0xA004,
    DB_HTILE_DATA_BASE = 0xA005,
    DB_STENCIL_CLEAR = 0xA00A,
    DB_DEPTH_CLEAR = 0xA00B,
    PA_SC_SCREEN_SCISSOR_TL = 0xA00C,
    PA_SC_SCREEN_SCISSOR_BR = 0xA00D,
    CB_COLOR0_BASE = 0xA010,
    CB_COLOR0_SIZE = 0xA018,
    CB_COLOR0_VIEW = 0xA020,
    CB_COLOR0_INFO = 0xA028,
    CB_COLOR0_MASK = 0xA040,
    PA_SC_WINDOW_OFFSET = 0xA080,
    PA_SC_WINDOW_SCISSOR_TL = 0xA081,
    PA_SC_WINDOW_SCISSOR_BR = 0xA082,
    CB_TARGET_MASK = 0xA08E,
    CB_SHADER_MASK = 0xA08F,
    PA_SC_GENERIC_SCISSOR_TL = 0xA090,
    PA_SC_GENERIC_SCISSOR_BR = 0xA091,
    SQ_VTX_SEMANTIC_0 = 0xA0E0,
    VGT_MAX_VTX_INDX = 0xA100,
    VGT_MIN_VTX_INDX = 0xA101,
    VGT_INDX_OFFSET = 0xA102,
    VGT_MULTI_PRIM_IB_RESET_INDX = 0xA103,
    SX_ALPHA_TEST_CONTROL = 0xA104,
    CB_BLEND_RED = 0xA105,
    DB_STENCILREFMASK = 0xA10C,
    DB_STENCILREFMASK_BF = 0xA10D,
    SX_ALPHA_REF = 0xA10E,
    PA_CL_VPORT_XSCALE = 0xA10F,
    SPI_VS_OUT_ID_0 = 0xA185,
    SPI_PS_INPUT_CNTL_0 = 0xA191,
    SPI_VS_OUT_CONFIG = 0xA1B1,
    SPI_THREAD_GROUPING = 0xA1B2,
    SPI_PS_IN_CONTROL_0 = 0xA1B3,
    SPI_PS_IN_CONTROL_1 = 0xA1B4,
    SPI_INTERP_CONTROL_0 = 0xA1B5,
    SPI_INPUT_Z = 0xA1B6,
    CB_BLEND0_CONTROL = 0xA1E0,
    CB_SHADER_CONTROL = 0xA1E8,
    DB_DEPTH_CONTROL = 0xA200,
    DB_SHADER_CONTROL = 0xA203,
    CB_COLOR_CONTROL = 0xA202,
    PA_CL_CLIP_CNTL = 0xA204,
    PA_SU_SC_MODE_CNTL = 0xA205,
    PA_CL_VTE_CNTL = 0xA206,
    PA_CL_VS_OUT_CNTL = 0xA207,
    SQ_PGM_START_PS = 0xA210,
    SQ_PGM_RESOURCES_PS = 0xA214,
    SQ_PGM_EXPORTS_PS = 0xA215,
    SQ_PGM_START_VS = 0xA216,
    SQ_PGM_RESOURCES_VS = 0xA21A,
    SQ_PGM_START_GS = 0xA21B,
    SQ_PGM_RESOURCES_GS = 0xA21F,
    SQ_PGM_START_ES = 0xA220,
    SQ_PGM_RESOURCES_ES = 0xA224,
    SQ_PGM_START_FS = 0xA225,
    SQ_PGM_RESOURCES_FS = 0xA229,
    SQ_ESGS_RING_ITEMSIZE = 0xA22A,
    SQ_GSVS_RING_ITEMSIZE = 0xA22B,
    SQ_GS_VERT_ITEMSIZE = 0xA232,
    SQ_VTX_SEMANTIC_CLEAR = 0xA238,
    PA_SU_POINT_SIZE = 0xA280,
    PA_SU_POINT_MINMAX = 0xA281,
    VGT_GS_MODE = 0xA290,
    VGT_GS_OUT_PRIM_TYPE = 0xA29B,
    VGT_DMA_INDEX_TYPE = 0xA29F,
    VGT_PRIMITIVEID_EN = 0xA2A1,
    VGT_DMA_NUM_INSTANCES = 0xA2A2,
    VGT_MULTI_PRIM_IB_RESET_EN = 0xA2A5,
    VGT_INSTANCE_STEP_RATE_0 = 0xA2A8,
    VGT_STRMOUT_EN = 0xA2AC,
    VGT_STRMOUT_BUFFER_SIZE_0 = 0xA2B4,
    VGT_STRMOUT_BUFFER_EN = 0xA2C8,
    PA_SU_POLY_OFFSET_DB_FMT_CNTL = 0xA37E,
    PA_SU_POLY_OFFSET_CLAMP = 0xA37F,
    PA_SU_POLY_OFFSET_FRONT_SCALE = 0xA380,

    SQ_TEX_RESOURCE_PS = 0xE000,           // 18 texture units x 7 words
    SQ_PS_UNIFORM_BLOCK = 0xE000 + 0x380,  // 16 blocks x 7 words (resources 128-143)
    SQ_TEX_RESOURCE_VS = 0xE460,
    SQ_VS_UNIFORM_BLOCK = 0xE000 + 0x7E0,
    SQ_VTX_ATTRIB_BUFFER = 0xE000 + 0x8C0, // 16 attribute buffers x 7 words
    SQ_TEX_RESOURCE_GS = 0xE930,
    SQ_GS_UNIFORM_BLOCK = 0xE000 + 0xCB0,
    SQ_TEX_SAMPLER0 = 0xF000,              // 18 per stage (PS 0, VS 18, GS 36) x 3 words
    SQ_VTX_BASE_VTX_LOC = 0xF3FC,
    SQ_VTX_START_INST_LOC = 0xF3FD,
};

#define LATTE_NUM_TEX_UNITS 18
#define LATTE_SAMPLER_BASE_VS 18
#define LATTE_SAMPLER_BASE_GS 36

// Index types (IT_INDEX_TYPE).
enum { LATTE_INDEX_U16_LE = 0, LATTE_INDEX_U32_LE = 1, LATTE_INDEX_U16_BE = 4, LATTE_INDEX_U32_BE = 9 };

// Primitive types (VGT_PRIMITIVE_TYPE).
enum {
    PRIM_POINTS = 1, PRIM_LINES = 2, PRIM_LINE_STRIP = 3, PRIM_TRIANGLES = 4,
    PRIM_TRIANGLE_FAN = 5, PRIM_TRIANGLE_STRIP = 6, PRIM_LINES_ADJ = 0xA,
    PRIM_LINE_STRIP_ADJ = 0xB, PRIM_TRIANGLES_ADJ = 0xC, PRIM_TRIANGLE_STRIP_ADJ = 0xD,
    PRIM_RECTS = 0x11, PRIM_LINE_LOOP = 0x12, PRIM_QUADS = 0x13, PRIM_QUAD_STRIP = 0x14,
};

// Surface formats as GX2 numbers them: the hardware format in the low 6 bits
// plus 0x100 integer, 0x200 signed, 0x400 sRGB, 0x800 float.
#define FMT_BIT_INT    0x100
#define FMT_BIT_SIGNED 0x200
#define FMT_BIT_SRGB   0x400
#define FMT_BIT_FLOAT  0x800

static inline uint32_t latte_bits(uint32_t v, int shift, int width) {
    return (v >> shift) & ((width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u));
}

#endif
