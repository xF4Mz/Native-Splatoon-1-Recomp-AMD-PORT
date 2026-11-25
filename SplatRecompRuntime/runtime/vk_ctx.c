// Vulkan setup (see vk_ctx.h). Needs Vulkan 1.3 (any graphics card from the
// last several years).
#include "vk_ctx.h"
#ifdef _WIN32
#include <windows.h>
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

VkCtx g_vk;

#define CHECK(call, what)                                                         \
    do {                                                                          \
        VkResult r_ = (call);                                                     \
        if (r_ != VK_SUCCESS) {                                                   \
            fprintf(stderr, "[vulkan] %s failed (error %d)\n", what, (int)r_);    \
            return false;                                                         \
        }                                                                         \
    } while (0)

uint32_t vkctx_memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t t = 0; t < g_vk.mem.memoryTypeCount; t++)
        if ((bits & (1u << t)) && (g_vk.mem.memoryTypes[t].propertyFlags & want) == want) return t;
    return UINT32_MAX;
}

static bool has_ext(VkPhysicalDevice gpu, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, NULL);
    VkExtensionProperties list[512];
    if (n > 512) n = 512;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, list);
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(list[i].extensionName, name) == 0) return true;
    return false;
}

static bool create(void* window) {
    if (volkInitialize() != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] couldn't load Vulkan (vulkan-1.dll); is the graphics driver installed?\n");
        return false;
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        fprintf(stderr, "[vulkan] the Vulkan on this PC is older than 1.3; please update the graphics driver\n");
        return false;
    }
    const char* exts[16];
    uint32_t n_ext = 0;
    if (window) {
        uint32_t n = 0;
        const char** req = glfwGetRequiredInstanceExtensions(&n);
        for (uint32_t i = 0; i < n && n_ext < 16; i++) exts[n_ext++] = req[i];
    }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Splatoon Recomp";
    app.pEngineName = "SplatRecompRuntime";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = n_ext;
    ici.ppEnabledExtensionNames = exts;
    CHECK(vkCreateInstance(&ici, NULL, &g_vk.instance), "creating the Vulkan instance");
    volkLoadInstance(g_vk.instance);
    if (window) {
        CHECK(glfwCreateWindowSurface(g_vk.instance, (GLFWwindow*)window, NULL, &g_vk.surface), "connecting Vulkan to the window");
        g_vk.window = window;
    }

    // Pick a graphics card: discrete first, must do Vulkan 1.3 + push descriptors.
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_vk.instance, &count, NULL);
    VkPhysicalDevice list[16];
    if (count > 16) count = 16;
    vkEnumeratePhysicalDevices(g_vk.instance, &count, list);
    int best = -1;
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(list[i], &p);
        const char* missing = p.apiVersion < VK_API_VERSION_1_3 ? "Vulkan 1.3"
                            : (window && !has_ext(list[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) ? "window output" : NULL;
        if (missing) {
            printf("[vulkan] skipping %s (Vulkan %u.%u): no %s\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                   VK_API_VERSION_MINOR(p.apiVersion), missing);
            continue;
        }
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(list[i], &qn, NULL);
        VkQueueFamilyProperties qp[32];
        if (qn > 32) qn = 32;
        vkGetPhysicalDeviceQueueFamilyProperties(list[i], &qn, qp);
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_TRUE;
            if (window) vkGetPhysicalDeviceSurfaceSupportKHR(list[i], q, g_vk.surface, &present);
            if (!(qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                      : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (score > best) { best = score; g_vk.gpu = list[i]; g_vk.queue_family = q; }
            break;
        }
    }
    if (best < 0) {
        fprintf(stderr, "[vulkan] no graphics card with Vulkan 1.3 found; please update the graphics driver\n");
        return false;
    }
    vkGetPhysicalDeviceProperties(g_vk.gpu, &g_vk.props);
    vkGetPhysicalDeviceMemoryProperties(g_vk.gpu, &g_vk.mem);
    printf("[vulkan] graphics card: %s (Vulkan %u.%u)\n", g_vk.props.deviceName,
           VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion));
#ifdef _WIN32
    {   // (what kind of PC this is, for the log)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        VkDeviceSize vram = 0;
        for (uint32_t i = 0; i < g_vk.mem.memoryHeapCount; i++)
            if (g_vk.mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += g_vk.mem.memoryHeaps[i].size;
        printf("[machine] %u logical processors, %.1f GB RAM, %s graphics with %.1f GB of its own memory%s\n", (unsigned)si.dwNumberOfProcessors,
               ms.ullTotalPhys / 1073741824.0, g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" : g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" : "other",
               vram / 1073741824.0, si.dwNumberOfProcessors <= 4 || ms.ullTotalPhys < (9ull << 30) ? " -- a small machine: Performance mode in the settings panel keeps the plaza at the game's own 30 fps" : "");
        printf("[engine] graphics engine -- Burned a bridge. Build a boat.\n");
    }
#endif

    // Features.
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    have.pNext = &f12;
    // (present id + present wait: knowing when a frame is on the screen, for the low-input-lag setting)
    VkPhysicalDevicePresentIdFeaturesKHR hpid = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
    VkPhysicalDevicePresentWaitFeaturesKHR hpw = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR };
    bool pw_exts = window && has_ext(g_vk.gpu, "VK_KHR_present_id") && has_ext(g_vk.gpu, "VK_KHR_present_wait");
    if (pw_exts) { f13.pNext = &hpid; hpid.pNext = &hpw; }
    // (a device lost explained: VK_EXT_device_fault gives the faulting address and kind; VK_EXT_robustness2 makes
    // out-of-range image / buffer reads return zero instead of killing the card. Hammerhead Bridge dropped the device
    // on its first frame; SPLAT_NO_ROBUST2=1 turns robustness2 off to see whether it was the cure)
    VkPhysicalDeviceFaultFeaturesEXT hflt = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
    VkPhysicalDeviceRobustness2FeaturesEXT hrob = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
    bool flt_ext = has_ext(g_vk.gpu, "VK_EXT_device_fault"), rob_ext = has_ext(g_vk.gpu, "VK_EXT_robustness2") && !getenv("SPLAT_NO_ROBUST2");
    // (pipeline libraries, Nov 10: shaders compiled once as parts, a new pairing of known shaders linked in well under a
    // millisecond -- the frames held in matches for "new" pipelines. Only with fast linking. SPLAT_GPL=0: off)
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT hgpl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT };
    bool gpl_ext = has_ext(g_vk.gpu, "VK_EXT_graphics_pipeline_library") && has_ext(g_vk.gpu, "VK_KHR_pipeline_library");
    // OFF unless SPLAT_GPL=1: the first launch with the feature enabled lost the device during the start-up warm-up
    // (Nov 10 ~23:50, GPU page fault, before any fast-linked pipeline was made) -- not understood yet; needs the
    // validation layer and the user's go before it's tried again.
    { const char* e = getenv("SPLAT_GPL"); if (!e || atoi(e) != 1) gpl_ext = false; }
    {   // (chained at the end of whatever is there)
        void** tail = pw_exts ? (void**)&hpw.pNext : (void**)&f13.pNext;
        if (flt_ext) { *tail = &hflt; tail = (void**)&hflt.pNext; }
        if (rob_ext) { *tail = &hrob; tail = (void**)&hrob.pNext; }
        if (gpl_ext) { *tail = &hgpl; tail = (void**)&hgpl.pNext; }
    }
    vkGetPhysicalDeviceFeatures2(g_vk.gpu, &have);
    f13.pNext = NULL;
    hpw.pNext = NULL;
    if (!f13.dynamicRendering) {
        fprintf(stderr, "[vulkan] the graphics driver lacks dynamic rendering; please update it\n");
        return false;
    }
    VkPhysicalDeviceVulkan13Features w13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    w13.dynamicRendering = VK_TRUE;
    w13.synchronization2 = f13.synchronization2;
    VkPhysicalDeviceVulkan12Features w12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    w12.samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge;
    w12.pNext = &w13;
    VkPhysicalDevicePresentIdFeaturesKHR wpid = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
    VkPhysicalDevicePresentWaitFeaturesKHR wpw = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR };
    g_vk.present_wait_ext = pw_exts && hpid.presentId && hpw.presentWait;
    if (g_vk.present_wait_ext) { wpid.presentId = VK_TRUE; wpw.presentWait = VK_TRUE; w13.pNext = &wpid; wpid.pNext = &wpw; }
    VkPhysicalDeviceFaultFeaturesEXT wflt = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
    VkPhysicalDeviceRobustness2FeaturesEXT wrob = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
    g_vk.device_fault = flt_ext && hflt.deviceFault;
    g_vk.robust2 = rob_ext && hrob.robustImageAccess2;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT wgpl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT };
    g_vk.gpl = false;
    if (gpl_ext && hgpl.graphicsPipelineLibrary) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &gp;
        vkGetPhysicalDeviceProperties2(g_vk.gpu, &p2);
        g_vk.gpl = gp.graphicsPipelineLibraryFastLinking != VK_FALSE;
    }
    {
        void** tail = g_vk.present_wait_ext ? (void**)&wpw.pNext : (void**)&w13.pNext;
        if (g_vk.device_fault) { wflt.deviceFault = VK_TRUE; *tail = &wflt; tail = (void**)&wflt.pNext; }
        if (g_vk.robust2) { wrob.robustImageAccess2 = VK_TRUE; wrob.robustBufferAccess2 = hrob.robustBufferAccess2; *tail = &wrob; tail = (void**)&wrob.pNext; }
        if (g_vk.gpl) { wgpl.graphicsPipelineLibrary = VK_TRUE; *tail = &wgpl; tail = (void**)&wgpl.pNext; }
    }
    printf("[vulkan] pipeline libraries (fast-linked pipelines for new shader pairings): %s\n", g_vk.gpl ? "on" : gpl_ext ? "not usable (no fast linking)" : "off");
    printf("[vulkan] device fault reports: %s; robust image access 2: %s\n", g_vk.device_fault ? "on" : "not in this driver", g_vk.robust2 ? "on" : rob_ext ? "off (SPLAT_NO_ROBUST2)" : "not in this driver");
    printf("[vulkan] present wait: %s\n", g_vk.present_wait_ext ? "available" : "not in this driver (low input lag then paces by the swapchain's image acquire)");
    VkPhysicalDeviceFeatures2 want = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    want.pNext = &w12;
    const VkPhysicalDeviceFeatures* h = &have.features;
    VkPhysicalDeviceFeatures* w = &want.features;
    w->robustBufferAccess = h->robustBufferAccess;   // stray shader reads return 0 instead of crashing the GPU
    w->textureCompressionBC = h->textureCompressionBC;
    w->independentBlend = h->independentBlend;
    w->depthClamp = h->depthClamp;
    w->samplerAnisotropy = h->samplerAnisotropy;
    w->dualSrcBlend = h->dualSrcBlend;
    w->logicOp = h->logicOp;
    w->fillModeNonSolid = h->fillModeNonSolid;
    w->wideLines = h->wideLines;
    w->largePoints = h->largePoints;
    w->shaderClipDistance = h->shaderClipDistance;
    w->imageCubeArray = h->imageCubeArray;
    w->geometryShader = h->geometryShader;
    w->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;   // stream out writes from vertex shaders
    g_vk.bc_textures = h->textureCompressionBC;
    g_vk.independent_blend = h->independentBlend;
    g_vk.depth_clamp = h->depthClamp;
    w->occlusionQueryPrecise = h->occlusionQueryPrecise;   // exact sample counts (the game counts ink with them)
    g_vk.precise_queries = h->occlusionQueryPrecise;
    g_vk.aniso = h->samplerAnisotropy;
    g_vk.dual_src = h->dualSrcBlend;
    g_vk.logic_op = h->logicOp;
    g_vk.mirror_clamp = f12.samplerMirrorClampToEdge;
    g_vk.wide_lines = h->wideLines;
    g_vk.wireframe = h->fillModeNonSolid;   // (the F7 wireframe views)

    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.gpu, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    g_vk.d24s8 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = g_vk.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dexts[12];
    uint32_t n_dext = 0;
    g_vk.push_descriptors = has_ext(g_vk.gpu, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    if (g_vk.push_descriptors) dexts[n_dext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    g_vk.max_push_desc = 0;
    if (g_vk.push_descriptors) {
        VkPhysicalDevicePushDescriptorPropertiesKHR pdp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &pdp;
        vkGetPhysicalDeviceProperties2(g_vk.gpu, &p2);
        g_vk.max_push_desc = pdp.maxPushDescriptors;
    }
    if (window) dexts[n_dext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (g_vk.present_wait_ext) { dexts[n_dext++] = "VK_KHR_present_id"; dexts[n_dext++] = "VK_KHR_present_wait"; }
    if (g_vk.device_fault) dexts[n_dext++] = "VK_EXT_device_fault";
    if (g_vk.robust2) dexts[n_dext++] = "VK_EXT_robustness2";
    if (g_vk.gpl) { dexts[n_dext++] = "VK_KHR_pipeline_library"; dexts[n_dext++] = "VK_EXT_graphics_pipeline_library"; }
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &want;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = n_dext;
    dci.ppEnabledExtensionNames = dexts;
    CHECK(vkCreateDevice(g_vk.gpu, &dci, NULL, &g_vk.device), "creating the Vulkan device");
    volkLoadDevice(g_vk.device);
    vkGetDeviceQueue(g_vk.device, g_vk.queue_family, 0, &g_vk.queue);
    return true;
}

// The settings panel's choices are kept in splat_settings.ini (in the working
// folder, next to cache\) and read back at the next start -- render scale,
// Performance mode and the rest used to start over every time. A setting whose
// environment variable is set (the launcher's switches, test runs) follows
// that instead. SPLAT_NO_SAVED_SETTINGS=1: neither read nor written.
typedef struct { const char* key; volatile int* v; const char* env; int lo, hi; } SettingDef;
static const SettingDef k_settings[] = {
    { "vsync", &g_vk.vsync_mode, NULL, 0, 2 },
    { "hide_fps", &g_vk.hide_fps, "SPLAT_HIDE_FPS", 0, 1 },
    { "anti_aliasing", &g_vk.aa_mode, "SPLAT_AA", 0, 1 },
    { "fov_add", &g_vk.fov_add, "SPLAT_FOV", 0, 40 },
    { "wide_screen", &g_vk.wide_mode, "SPLAT_WIDE", 0, 1 },
    { "hud_stretched", &g_vk.hud_mode, "SPLAT_HUD", 0, 1 },
    { "gamepad_size", &g_vk.drc_size, NULL, 0, 3 },
    { "gamepad_cursor_speed", &g_vk.drc_cursor_speed, NULL, 0, 3 },
    { "low_input_lag", &g_vk.low_latency, "SPLAT_LOWLAT", 0, 1 },
    { "volume", &g_vk.volume, NULL, 0, 100 },
    { "gamepad_popup_off", &g_vk.drc_no_popup, "SPLAT_DRC_POPUP", 0, 1 },
    { "button_layout", &g_vk.pad_layout, "SPLAT_PAD_LAYOUT", 0, 1 },
    { "settings_box_hidden", &g_vk.hide_settings, NULL, 0, 1 },
    { "plaza_30fps", &g_vk.plaza_30, "SPLAT_PLAZA_30", 0, 1 },
    { "emote_sound", &g_vk.emote_sound, "SPLAT_EMOTE_SOUND", 0, 1 },
    { "shadow_quality", &g_vk.shadow_quality, "SPLAT_SHADOWS", 0, 2 },
    { "sky_light", &g_vk.sky_mode, "SPLAT_SKY", 0, 2 },
    { "shader_clouds", &g_vk.sh_clouds, "SPLAT_SH_CLOUDS", 0, 1 },
    { "shader_grade", &g_vk.sh_grade, "SPLAT_SH_GRADE", 0, 1 },
    { "motion_blur", &g_vk.motion_blur, "SPLAT_MOTION_BLUR", 0, 2 },
    { "dojo_both_views", &g_vk.dojo_both, "SPLAT_DOJO_BOTH", 0, 1 },
    { "shader_quality", &g_vk.sh_quality, "SPLAT_SH_QUALITY", 0, 2 },
    { "shader_god_rays", &g_vk.sh_rays, "SPLAT_SH_RAYS", 0, 1 },
    { "shader_cloud_shadows", &g_vk.sh_cshadow, "SPLAT_SH_CSHADOW", 0, 1 },
    { "shader_ambient_occlusion", &g_vk.sh_ao, "SPLAT_SH_AO", 0, 1 },
    { "shader_rain", &g_vk.sh_rain, "SPLAT_SH_RAIN", 0, 1 },
    { "online", &g_vk.online, "SPLAT_ONLINE", 0, 1 },
    { "player2_pro_controller", &g_vk.pro_pad, "SPLAT_PRO_PAD", 0, 3 },
    { "warm_shaders", &g_vk.warm_shaders, "SPLAT_WARM", 0, 1 },
    { "new_shaders_wait", &g_vk.build_wait, "SPLAT_BUILD_WAIT", 0, 1 },
    { "smooth_motion", &g_vk.frame_gen, "SPLAT_FRAMEGEN", 0, 4 },
    { "mouse_aim", &g_vk.mouse_aim, "SPLAT_MOUSE_AIM", 0, 4 },
    { "motion_controller", &g_vk.gyro_on, "SPLAT_GYRO", 0, 1 },
};
// (the Bot Lobby keeps its own: player 2 = the bot must never carry over to normal play; its first start reads the
// normal file, so the player's graphics choices come along)
int splat_lobby_on(void);
static const char* settings_file(void) { return splat_lobby_on() ? "splat_settings_botlobby.ini" : "splat_settings.ini"; }
#define SETTINGS_FILE settings_file()
// (Performance mode and normal play each keep their own render scale: Performance
// mode starts at 0.5x. The launcher always says whether Performance mode is on,
// SPLAT_PERF_MODE; without it the saved choice holds.)
static int s_rs_normal = 0, s_rs_perf = 3;
static void settings_apply_perf(int perf) {
    g_vk.perf_mode = perf ? 1 : 0;
    g_vk.render_scale = perf ? s_rs_perf : s_rs_normal;   // (SPLAT_RENDER_SCALE wins: latte_render.c rs_factor)
    if (perf) g_vk.aa_mode = 0;
}
void vkctx_perf_set(int on) {
    if (g_vk.perf_mode) s_rs_perf = g_vk.render_scale; else s_rs_normal = g_vk.render_scale;
    settings_apply_perf(on);
}
static void settings_load(void) {
    const char* pe = getenv("SPLAT_PERF_MODE");
    int perf = pe && atoi(pe);
    FILE* f = getenv("SPLAT_NO_SAVED_SETTINGS") ? NULL : fopen(SETTINGS_FILE, "r");
    if (!f && splat_lobby_on() && !getenv("SPLAT_NO_SAVED_SETTINGS")) f = fopen("splat_settings.ini", "r");
    if (!f) { settings_apply_perf(perf); return; }
    char line[128];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "render_scale") && v >= 0 && v <= 4) { s_rs_normal = v; n++; }
        if (!strcmp(line, "perf_render_scale") && v >= 0 && v <= 4) { s_rs_perf = v; n++; }
        if (!strcmp(line, "performance_mode") && !pe) { perf = v != 0; n++; }
        for (size_t i = 0; i < sizeof(k_settings) / sizeof(k_settings[0]); i++) {
            const SettingDef* d = &k_settings[i];
            if (strcmp(line, d->key) || (d->env && getenv(d->env))) continue;
            int v = atoi(eq + 1);
            if (v >= d->lo && v <= d->hi) { *d->v = v; n++; }
        }
    }
    fclose(f);
    settings_apply_perf(perf);
    printf("[display] %d settings read from %s\n", n, SETTINGS_FILE);
}
void vkctx_settings_save(void) {
    if (getenv("SPLAT_NO_SAVED_SETTINGS")) return;
    { extern bool splat_bench_active(void); if (splat_bench_active()) return; }   // (the benchmark forces player 2 and no HUD: not the player's settings)
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", SETTINGS_FILE);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "; the settings panel's choices (F10 or F2 in the game), written by the game -- an environment variable for one wins\n");
    if (g_vk.perf_mode) s_rs_perf = g_vk.render_scale; else s_rs_normal = g_vk.render_scale;
    fprintf(f, "performance_mode=%d\nrender_scale=%d\nperf_render_scale=%d\n", g_vk.perf_mode, s_rs_normal, s_rs_perf);
    for (size_t i = 0; i < sizeof(k_settings) / sizeof(k_settings[0]); i++) fprintf(f, "%s=%d\n", k_settings[i].key, *k_settings[i].v);
    if (fclose(f) == 0) { remove(SETTINGS_FILE); rename(tmp, SETTINGS_FILE); }
}

bool vkctx_create(void* window) {
    g_vk.volume = 100;
    g_vk.low_latency = 1;   // (on by default: the plaza bench was no worse with it, 61.9 vs 57.3 fps, and the lag is one frame)
    g_vk.emote_sound = 1;
    g_vk.shadow_quality = 2;
    g_vk.sh_clouds = 1;
    g_vk.sh_grade = 1;
    g_vk.dojo_both = 1;
    g_vk.sh_quality = 1;
    g_vk.sh_rays = 1;
    g_vk.sh_cshadow = 1;
    g_vk.sh_ao = 1;
    g_vk.sh_rain = 1;
    g_vk.warm_shaders = 1;
    g_vk.build_wait = 1;
    g_vk.mouse_aim = 0;   // (off until picked: a click on the picture would otherwise hide the cursor)
    g_vk.gyro_on = 1;
    settings_load();
    for (size_t i = 0; i < sizeof(k_settings) / sizeof(k_settings[0]); i++) {   // (the SHADERS rows' variables: nothing else reads them)
        const SettingDef* d = &k_settings[i];
        const char* e = d->env && !strncmp(d->env, "SPLAT_SH_", 9) ? getenv(d->env) : NULL;
        if (e && atoi(e) >= d->lo && atoi(e) <= d->hi) *d->v = atoi(e);
    }
    { const char* e = getenv("SPLAT_VOLUME"); if (e) g_vk.volume = atoi(e); }   // (test runs: SPLAT_VOLUME=0, silent)
    { extern void splat_emote_init(void); splat_emote_init(); }
    { extern void gyro_start(void); gyro_start(); }
    bool ok = create(window);
    g_vk.state = ok ? 1 : -1;
    return ok;
}
