// Assorted system calls: calendar time, the locked cache, ProcUI (the
// foreground/background lifecycle), system settings, user accounts.
#include "sys.h"
#include "wiiu_mem.h"
#include "guest_threads.h"

#include <stdio.h>
#include <string.h>

// --- Calendar time ---------------------------------------------------------
// OSCalendarTime: sec, min, hour, mday, mon (0-11), year, wday, yday, msec, usec.
static void days_to_civil(long long z, int* y, int* m, int* d) {
    // Howard Hinnant's algorithm, days since 1970-01-01.
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}
static long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static void OSTicksToCalendarTime(CPUState* c) {   // (ticks hi, lo, *out)
    u64 ticks = ((u64)arg(c, 0) << 32) | arg(c, 1);
    u32 out = arg(c, 2);
    u64 total_sec = ticks / WIIU_TIMER_HZ;
    u64 rem = ticks % WIIU_TIMER_HZ;
    long long days = (long long)(total_sec / 86400) + 10957;   // 2000-01-01 in days since 1970
    u32 secs = (u32)(total_sec % 86400);
    int y, m, d;
    days_to_civil(days, &y, &m, &d);
    u64 usec_total = rem * 1000000ull / WIIU_TIMER_HZ;
    wiiu_write32(out + 0x00, secs % 60);
    wiiu_write32(out + 0x04, (secs / 60) % 60);
    wiiu_write32(out + 0x08, secs / 3600);
    wiiu_write32(out + 0x0C, (u32)d);
    wiiu_write32(out + 0x10, (u32)(m - 1));
    wiiu_write32(out + 0x14, (u32)y);
    wiiu_write32(out + 0x18, (u32)((days % 7 + 11) % 7));   // 1970-01-01 was a Thursday (4)
    wiiu_write32(out + 0x1C, (u32)(days - days_from_civil(y, 1, 1)));
    wiiu_write32(out + 0x20, (u32)(usec_total / 1000));
    wiiu_write32(out + 0x24, (u32)(usec_total % 1000));
}
static void OSCalendarTimeToTicks(CPUState* c) {   // (*in) -> u64 ticks in r3:r4
    u32 in = arg(c, 0);
    int sec = (int)wiiu_read32(in), min = (int)wiiu_read32(in + 4), hour = (int)wiiu_read32(in + 8);
    int mday = (int)wiiu_read32(in + 0xC), mon = (int)wiiu_read32(in + 0x10), year = (int)wiiu_read32(in + 0x14);
    int msec = (int)wiiu_read32(in + 0x20), usec = (int)wiiu_read32(in + 0x24);
    long long days = days_from_civil(year, mon + 1, mday) - 10957;
    long long s = days * 86400 + hour * 3600 + min * 60 + sec;
    u64 t = (u64)s * WIIU_TIMER_HZ + ((u64)msec * 1000 + (u64)usec) * WIIU_TIMER_HZ / 1000000ull;
    c->gpr[3] = (u32)(t >> 32);
    c->gpr[4] = (u32)t;
}

// --- Cache helpers -------------------------------------------------------------
static void DCZeroRange(CPUState* c) {   // (addr, size): zero whole 32-byte lines
    u32 start = arg(c, 0) & ~31u;
    u32 end = (arg(c, 0) + arg(c, 1) + 31u) & ~31u;
    u8* p = wiiu_mem_ptr(start, end - start ? end - start : 1);
    if (p) memset(p, 0, end - start);
    wiiu_mem_mark(start, end - start);
}

// Locked cache: 16 KB of fast scratch memory per core, filled by DMA.
// Ordinary memory does the job here.
static void LCAlloc(CPUState* c) { ret(c, hle_heap_alloc(arg(c, 0) ? arg(c, 0) : 32, 512)); }
static void LCDealloc(CPUState* c) { hle_heap_free(arg(c, 0)); }
static void LCGetUnallocated(CPUState* c) { ret(c, 0x4000); }
static void LCHardwareIsAvailable(CPUState* c) { ret(c, 1); }
static void LCIsDMAEnabled(CPUState* c) { ret(c, 1); }
static void LCEnableDMA(CPUState* c) { ret(c, 1); }
static void LCCopyBlocks(CPUState* c) {   // (dst, src, blocks): 32-byte blocks, 0 = 128
    u32 n = arg(c, 2) ? arg(c, 2) : 128;
    u8* d = wiiu_mem_ptr(arg(c, 0), n * 32);
    u8* s = wiiu_mem_ptr(arg(c, 1), n * 32);
    if (d && s) memmove(d, s, n * 32);
}

// --- ProcUI: the app's foreground/background lifecycle ---------------------
static void ProcUIProcessMessages(CPUState* c) { ret(c, 0); }   // in foreground
static void ProcUIIsRunning(CPUState* c) { ret(c, 1); }
static void ProcUIInForeground(CPUState* c) { ret(c, 1); }

// --- System settings ---------------------------------------------------------
// UCReadSysConfig(handle, count, settings): each setting is 0x54 bytes:
// name[64], access, dataType, error, dataSize, data pointer.
static void UCReadSysConfig(CPUState* c) {
    u32 count = arg(c, 1), list = arg(c, 2);
    for (u32 i = 0; i < count; i++) {
        u32 s = list + i * 0x54;
        char name[65];
        wiiu_read_string(s, name, sizeof(name));
        u32 size = wiiu_read32(s + 0x4C), data = wiiu_read32(s + 0x50);
        u8* p = data ? wiiu_mem_ptr(data, size ? size : 1) : NULL;
        if (p && size) memset(p, 0, size);
        u32 value = 0;
        if (!strcmp(name, "cafe.language")) value = 1;          // English
        else if (!strcmp(name, "cafe.cntry_reg")) value = 49;   // United States
        else if (!strcmp(name, "cafe.version")) value = 0x0F;
        if (p && size >= 4) wiiu_write32(data, value);
        else if (p && size == 2) wiiu_write16(data, (u16)value);
        else if (p && size == 1) wiiu_write8(data, (u8)value);
        wiiu_write32(s + 0x48, 0);   // no error
        static int logged = 0;
        if (logged++ < 32) printf("[sys] setting %s -> %u\n", name, value);
    }
    ret(c, 0);
}
static void UCOpen(CPUState* c) { ret(c, 1); }
static void MCP_Open(CPUState* c) { ret(c, 1); }
static void MCP_GetSysProdSettings(CPUState* c) {   // (handle, *settings)
    u32 s = arg(c, 1);
    for (u32 i = 0; i < 0x46; i += 2) wiiu_write16(s + i, 0);
    wiiu_write32(s + 0x00, 2);   // product area: USA
    wiiu_write32(s + 0x08, 2);   // game region: USA
    ret(c, 0);
}

// --- Accounts (nn_act): one local user, no Nintendo Network ID -------------
static void act_slot(CPUState* c) { ret(c, 1); }
static void act_true(CPUState* c) { ret(c, 1); }
static void act_false(CPUState* c) { ret(c, 0); }
static void act_network_account(CPUState* c) { extern int splat_online_local(void); ret(c, splat_online_local() ? 1 : 0); }   // (always 0 here: no online in this release)
static void act_persistent_id(CPUState* c) { ret(c, 0x80000001u); }
static void act_mii_name(CPUState* c) {   // (wchar16* out[11])
    static const char name[] = "Player";
    u32 out = arg(c, 0);
    for (u32 i = 0; i <= sizeof(name) - 1; i++) wiiu_write16(out + i * 2, (u16)name[i]);
    ret(c, 0);
}
static void act_country(CPUState* c) {    // (char* out)
    u8* p = wiiu_mem_ptr(arg(c, 0), 3);
    if (p) memcpy(p, "US", 3);
    ret(c, 0);
}

// A valid default Mii for the account (FFLStoreData, 0x60 bytes). Wii U Mii
// data is the 3DS layout with each field stored big-endian, so the first
// word is: birth platform (Wii U = 4) in the top nibble, font region /
// copyable flags, then the Mii data version (3) in the last byte. The
// create ID starts with flags 0xD (normal Mii made on a Wii U). The game
// checks a CRC-16/XMODEM over the first 0x5E bytes, stored at 0x5E.
static u16 crc16_xmodem(const u8* p, u32 n) {
    u16 crc = 0;
    for (u32 i = 0; i < n; i++) {
        crc ^= (u16)(p[i] << 8);
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
    }
    return crc;
}
void sys_write_default_mii(u32 out) {
    u8 m[0x60];
    memset(m, 0, sizeof(m));
    m[0x00] = 0x40;                       // birth platform: Wii U
    m[0x01] = 0x00;
    m[0x02] = 0x01;                       // copyable
    m[0x03] = 0x03;                       // Mii data version 3
    for (int i = 0; i < 8; i++) m[0x04 + i] = (u8)(0x11 * (i + 1));   // author (system) ID
    m[0x0C] = 0xD0; m[0x0D] = 0x12; m[0x0E] = 0x34; m[0x0F] = 0x56;   // create ID: flags + time
    m[0x10] = 0x02; m[0x11] = 0x11; m[0x12] = 0x22; m[0x13] = 0x33; m[0x14] = 0x44; m[0x15] = 0x55;   // + device
    static const char name[] = "Player";
    for (int i = 0; name[i]; i++) { m[0x1A + i * 2] = 0; m[0x1B + i * 2] = (u8)name[i]; }   // name (UTF-16BE)
    m[0x2E] = 64;                         // height
    m[0x2F] = 64;                         // build
    for (int i = 0; name[i]; i++) { m[0x48 + i * 2] = 0; m[0x49 + i * 2] = (u8)name[i]; }   // creator name
    u16 crc = crc16_xmodem(m, 0x5E);
    m[0x5E] = (u8)(crc >> 8);
    m[0x5F] = (u8)crc;
    u8* dst = wiiu_mem_ptr(out, 0x60);
    if (dst) memcpy(dst, m, 0x60);
}
static void act_get_mii(CPUState* c) { sys_write_default_mii(arg(c, 0)); ret(c, 0); }      // (FFLStoreData*)
static void act_get_mii_ex(CPUState* c) { sys_write_default_mii(arg(c, 0)); ret(c, 0); }   // (FFLStoreData*, slot)
static void act_transferable_id(CPUState* c) {   // (u64* out, uniqueId, slot)
    if (arg(c, 0)) { wiiu_write32(arg(c, 0), 0x01234567u); wiiu_write32(arg(c, 0) + 4, 0x89ABCDEFu); }
    ret(c, 0);
}

static void nothing(CPUState* c) { (void)c; }
static void zero(CPUState* c) { ret(c, 0); }

// Title ID of a system app (Mii Maker, eShop...). (type) -> u64 in r3:r4
static void SYSGetSystemApplicationTitleId(CPUState* c) {
    u32 type = arg(c, 0);
    c->gpr[3] = 0x00050010u;
    c->gpr[4] = 0x10000000u | (type << 8);
}
// The console's built-in fonts: 0 Chinese, 1 Korean, 2 standard
// (Latin + Japanese), 3 Taiwanese. They live in the console's system
// storage, not in the game; games that draw text with them (Splatoon's
// "scalable font") can't finish starting up without one. We hand the game a
// TrueType font from the PC: a `fonts` folder next to where the runner
// starts (CafeStd.ttf etc., if you dumped your own console's), otherwise a
// font Windows (or Linux) already has.
static u32 g_font_addr[4], g_font_size[4];

static bool font_from_file(int id, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u32 addr = size > 0 && size < (64l << 20) ? hle_heap_alloc((u32)size, 0x100) : 0;
    u8* p = addr ? wiiu_mem_ptr(addr, (u32)size) : NULL;
    bool ok = p && fread(p, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) { if (addr) hle_heap_free(addr); return false; }
    g_font_addr[id] = addr;
    g_font_size[id] = (u32)size;
    printf("[sys] system font %d: %s (%ld KB)\n", id, path, size / 1024);
    return true;
}

static bool load_system_font(int id) {
    static const char* own[4] = { "fonts/CafeCn.ttf", "fonts/CafeKr.ttf", "fonts/CafeStd.ttf", "fonts/CafeTw.ttf" };
    static const char* fallback[] = {
        "fonts/CafeStd.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
    if (g_font_addr[id]) return true;
    for (int i = 0; i < 4; i++)   // already loaded under another id: share it
        if (g_font_addr[i] && i != id) {
            g_font_addr[id] = g_font_addr[i];
            g_font_size[id] = g_font_size[i];
            return true;
        }
    if (font_from_file(id, own[id])) return true;
    for (size_t i = 0; i < sizeof(fallback) / sizeof(fallback[0]); i++)
        if (font_from_file(id, fallback[i])) return true;
    printf("[sys] no system font found (put a .ttf font at fonts/CafeStd.ttf)\n");
    return false;
}

static void OSGetSharedData(CPUState* c) {   // (id, flags, **addr, *size) -> found
    u32 id = arg(c, 0);
    bool ok = id < 4 && load_system_font((int)id);
    if (arg(c, 2)) wiiu_write32(arg(c, 2), ok ? g_font_addr[id] : 0);
    if (arg(c, 3)) wiiu_write32(arg(c, 3), ok ? g_font_size[id] : 0);
    ret(c, ok ? 1 : 0);
}

// --- Overlay arena: extra memory a game can turn on ---------------------------
static bool g_overlay_enabled = false;
static void overlay_range(u32 addr_out, u32 size_out) {
    if (addr_out) wiiu_write32(addr_out, WIIU_OVERLAY_BASE);
    if (size_out) wiiu_write32(size_out, WIIU_OVERLAY_SIZE);
}
static void OSIsEnabledOverlayArena(CPUState* c) { ret(c, g_overlay_enabled ? 1 : 0); }
static void OSEnableOverlayArena(CPUState* c) {   // (unknown, *addr, *size)
    g_overlay_enabled = true;
    overlay_range(arg(c, 1), arg(c, 2));
    ret(c, 0);
}
static void OSDisableOverlayArena(CPUState* c) { g_overlay_enabled = false; ret(c, 0); }
static void OSGetOverlayArenaRange(CPUState* c) { overlay_range(arg(c, 0), arg(c, 1)); }

// The system loader's own memory (for loading system libraries on demand).
// Games check there's room before opening things like the error viewer.
static void OSDynLoad_GetLoaderHeapStatistics(CPUState* c) {   // (*stats)
    u32 s = arg(c, 0);
    if (s) {
        wiiu_write32(s + 0x00, 0x00400000u);   // code heap used
        wiiu_write32(s + 0x04, 0);
        wiiu_write32(s + 0x08, 0x04000000u);   // code heap free (64 MB)
        wiiu_write32(s + 0x0C, 0x04000000u);   // largest free block
        wiiu_write32(s + 0x10, 0x00200000u);   // data heap used
        wiiu_write32(s + 0x14, 0);
    }
    ret(c, 0);
}

static const HleEntry g_sys_table[] = {
    { "OSTicksToCalendarTime", OSTicksToCalendarTime },
    // nn_acp (network time, s64 + *out -- the same as OSTicks here, since
    // ACPGetNetworkTime hands out OS ticks). It used to return without
    // filling the date in: the game then counted days from 1970 up to a
    // garbage year one by one -- about 20 seconds of black screen at boot.
    { "ACPConvertNetworkTimeToOSCalendarTime", OSTicksToCalendarTime },
    { "OSCalendarTimeToTicks", OSCalendarTimeToTicks },
    { "DCZeroRange", DCZeroRange },

    { "LCAlloc", LCAlloc },
    { "LCDealloc", LCDealloc },
    { "LCGetUnallocated", LCGetUnallocated },
    { "LCHardwareIsAvailable", LCHardwareIsAvailable },
    { "LCIsDMAEnabled", LCIsDMAEnabled },
    { "LCEnableDMA", LCEnableDMA },
    { "LCDisableDMA", nothing },
    { "LCLoadDMABlocks", LCCopyBlocks },
    { "LCStoreDMABlocks", LCCopyBlocks },
    { "LCWaitDMAQueue", nothing },

    { "ProcUIInit", nothing },
    { "ProcUIInitEx", nothing },
    { "ProcUIRegisterCallback", nothing },
    { "ProcUIProcessMessages", ProcUIProcessMessages },
    { "ProcUISubProcessMessages", ProcUIProcessMessages },
    { "ProcUIIsRunning", ProcUIIsRunning },
    { "ProcUIInForeground", ProcUIInForeground },
    { "ProcUIDrawDoneRelease", nothing },
    { "ProcUIShutdown", nothing },
    { "OSSavesDone_ReadyToRelease", nothing },

    { "UCOpen", UCOpen },
    { "UCClose", nothing },
    { "UCReadSysConfig", UCReadSysConfig },
    { "MCP_Open", MCP_Open },
    { "MCP_Close", nothing },
    { "MCP_GetSysProdSettings", MCP_GetSysProdSettings },

    { "Initialize__Q2_2nn3actFv", act_false },
    { "Finalize__Q2_2nn3actFv", act_false },
    { "GetSlotNo__Q2_2nn3actFv", act_slot },
    { "GetDefaultAccount__Q2_2nn3actFv", act_slot },
    { "IsSlotOccupied__Q2_2nn3actFUc", act_true },
    { "IsNetworkAccount__Q2_2nn3actFv", act_network_account },
    { "HasNfsAccount__Q2_2nn3actFv", act_true },   // (if not, Splatoon tries to register one)
    { "GetPersistentId__Q2_2nn3actFv", act_persistent_id },
    { "GetPersistentIdEx__Q2_2nn3actFUc", act_persistent_id },
    { "GetMiiName__Q2_2nn3actFPw", act_mii_name },
    { "GetCountry__Q2_2nn3actFPc", act_country },
    { "GetMii__Q2_2nn3actFP12FFLStoreData", act_get_mii },
    { "GetMiiEx__Q2_2nn3actFP12FFLStoreDataUc", act_get_mii_ex },
    { "GetTransferableIdEx__Q2_2nn3actFPULUiUc", act_transferable_id },

    { "_SYSGetSystemApplicationTitleId", SYSGetSystemApplicationTitleId },
    { "OSGetSharedData", OSGetSharedData },

    { "OSIsEnabledOverlayArena", OSIsEnabledOverlayArena },
    { "OSEnableOverlayArena", OSEnableOverlayArena },
    { "OSDisableOverlayArena", OSDisableOverlayArena },
    { "OSGetOverlayArenaRange", OSGetOverlayArenaRange },
    { "OSDynLoad_GetLoaderHeapStatistics", OSDynLoad_GetLoaderHeapStatistics },

    // Hardware registers and drivers the game pokes at (GPU/system tuning):
    // nothing to do on a PC.
    { "OSSetExceptionCallbackEx", zero },
    { "OSSetExceptionCallback", zero },
    { "OSReadRegister16", zero },
    { "OSWriteRegister16", zero },
    { "__OSReadRegister32Ex", zero },
    { "__OSWriteRegister32Ex", zero },
    { "OSDriver_Register", zero },
    { "OSDriver_Deregister", zero },
};

const HleEntry* sys_hle_table(size_t* count) {
    *count = sizeof(g_sys_table) / sizeof(g_sys_table[0]);
    return g_sys_table;
}
