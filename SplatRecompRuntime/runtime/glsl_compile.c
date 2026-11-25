// GLSL -> SPIR-V with glslang (the Khronos reference compiler, bundled in
// third_party/glslang), through its C interface.
#include "glsl_compile.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool g_ready = false;

static void print_numbered(const char* src) {
    int n = 1;
    const char* p = src;
    while (*p) {
        const char* e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        fprintf(stderr, "%4d: %.*s\n", n++, len, p);
        if (!e) break;
        p = e + 1;
    }
}

void glsl_init(void) {
    if (!g_ready) {
        glslang_initialize_process();
        g_ready = true;
    }
}

// A "// spv-source-extension: <text>" line in the source goes into the SPIR-V as an
// OpSourceExtension "<text>" (after OpSource): drivers ignore it, and the renderer reads
// notes like this back from cached shaders (latte_render.c shader_note_ub)
static u32* add_source_extension(const char* src, u32* spv, size_t* n) {
    static const char tag[] = "\n// spv-source-extension: ";
    const char* at = strstr(src, tag);
    if (!at || at - src > 4096) return spv;
    at += sizeof(tag) - 1;
    size_t len = strcspn(at, "\r\n");
    if (!len || len > 1000) return spv;
    size_t sw = (len + 4) / 4;   // (the string's words, with its terminating zero)
    size_t iw = 1 + sw;
    size_t ins = 0;
    for (size_t i = 5; i < *n && i < 4096; ) {
        u32 wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc || i + wc > *n) return spv;
        if (op == 3) {   // OpSource (and any OpSourceContinued right after it)
            ins = i + wc;
            while (ins < *n && (spv[ins] & 0xFFFF) == 2 && (spv[ins] >> 16)) ins += spv[ins] >> 16;
            break;
        }
        i += wc;
    }
    if (!ins) return spv;
    u32* o = (u32*)malloc((*n + iw) * sizeof(u32));
    if (!o) return spv;
    memcpy(o, spv, ins * sizeof(u32));
    o[ins] = (u32)(iw << 16) | 4u;   // OpSourceExtension
    memset(o + ins + 1, 0, sw * sizeof(u32));
    memcpy(o + ins + 1, at, len);   // (SPIR-V strings: bytes in order, lowest byte of each word first)
    memcpy(o + ins + iw, spv + ins, (*n - ins) * sizeof(u32));
    free(spv);
    *n += iw;
    return o;
}

u32* glsl_to_spirv(bool fragment, const char* src, size_t* words) {
    glsl_init();
    *words = 0;
    glslang_input_t in;
    memset(&in, 0, sizeof(in));
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();

    glslang_shader_t* sh = glslang_shader_create(&in);
    if (!sh) return NULL;
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        static int shown = 0;
        fprintf(stderr, "[shader] GLSL compile failed: %s\n", glslang_shader_get_info_log(sh));
        if (shown++ < 2) print_numbered(src);
        glslang_shader_delete(sh);
        return NULL;
    }
    glslang_program_t* prog = glslang_program_create();
    glslang_program_add_shader(prog, sh);
    if (!glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        fprintf(stderr, "[shader] GLSL link failed: %s\n", glslang_program_get_info_log(prog));
        glslang_program_delete(prog);
        glslang_shader_delete(sh);
        return NULL;
    }
    glslang_program_SPIRV_generate(prog, in.stage);
    size_t n = glslang_program_SPIRV_get_size(prog);
    u32* out = (u32*)malloc(n * sizeof(u32));
    if (out) {
        glslang_program_SPIRV_get(prog, out);
        out = add_source_extension(src, out, &n);
        *words = n;
    }
    glslang_program_delete(prog);
    glslang_shader_delete(sh);
    return out;
}
