// Motion controls from a real controller: the gyro of a PlayStation pad (DualShock 4, DualSense,
// DualSense Edge), read straight from Windows' HID driver on a thread of its own, over USB or Bluetooth.
// The game aims by turning the Wii U GamePad (input.c VPADRead keeps a virtual one): this adds the pad's
// real turn rates to it. Buttons and sticks still come through GLFW as before; only the motion comes from here.
// Settings "Motion controller" (gyro_on) / SPLAT_GYRO=0 turns it off. Our own code, from the controllers'
// published report layouts.
#include "vk_ctx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

typedef struct { USHORT pid; int kind; const char* name; } PadModel;   // (kind 1 = DualShock 4, 2 = DualSense)
static const PadModel k_models[] = {
    { 0x05C4, 1, "DualShock 4" }, { 0x09CC, 1, "DualShock 4" }, { 0x0BA0, 1, "DualShock 4 (wireless adapter)" },
    { 0x0CE6, 2, "DualSense" }, { 0x0DF2, 2, "DualSense Edge" },
};

static CRITICAL_SECTION g_cs;
static float g_acc_pitch, g_acc_yaw;   // degrees turned since the last gyro_take
static volatile int g_found;            // a pad is open
static const char* g_name = "";
volatile int splat_gyro_reports;        // (reports read: the log line)

// One report -> turn rates in degrees per second (pitch: nose up +, yaw: turning left +).
// DualShock 4: USB report 0x01 (gyro at 13), Bluetooth 0x11 (at 15). DualSense: USB 0x01 (gyro at 16),
// Bluetooth 0x31 (at 17). Full scale +-2000 deg/s over 16 bits: 16.4 units per deg/s.
static int parse(int kind, const BYTE* r, DWORD n, float* gx, float* gy, float* gz) {
    int at = -1;
    if (kind == 1) at = r[0] == 0x01 && n >= 25 ? 13 : r[0] == 0x11 && n >= 27 ? 15 : -1;
    else at = r[0] == 0x01 && n >= 28 ? 16 : r[0] == 0x31 && n >= 29 ? 17 : -1;
    if (at < 0) return 0;
    short x = (short)(r[at] | r[at + 1] << 8), y = (short)(r[at + 2] | r[at + 3] << 8), z = (short)(r[at + 4] | r[at + 5] << 8);
    *gx = x / 16.4f; *gy = y / 16.4f; *gz = z / 16.4f;
    return 1;
}

static HANDLE open_pad(int* kind, const char** name) {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, NULL, 0, &need, NULL);
        if (!need || need > 4096) continue;
        BYTE buf[4096];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, NULL, NULL)) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a;
        a.Size = sizeof(a);
        int k = 0;
        if (HidD_GetAttributes(h, &a) && a.VendorID == 0x054C)
            for (size_t m = 0; m < sizeof(k_models) / sizeof(k_models[0]); m++)
                if (k_models[m].pid == a.ProductID) { k = k_models[m].kind; *name = k_models[m].name; }
        if (k) { found = h; *kind = k; }
        else CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (found != INVALID_HANDLE_VALUE) {
        // (over Bluetooth both pads send a short report without motion until a feature report is read: the
        // calibration data -- DualShock 4: 0x02 over USB, 0x05 over Bluetooth; DualSense: 0x05. Harmless over USB.
        // The buffer must be the device's own feature-report length.)
        USHORT flen = 64;
        PHIDP_PREPARSED_DATA pp;
        if (HidD_GetPreparsedData(found, &pp)) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.FeatureReportByteLength) flen = caps.FeatureReportByteLength;
            HidD_FreePreparsedData(pp);
        }
        BYTE f[1024];
        if (flen > sizeof(f)) flen = sizeof(f);
        static const BYTE ids[2][2] = { { 0x02, 0x05 }, { 0x05, 0x05 } };
        for (int i = 0; i < 2; i++) {
            memset(f, 0, sizeof(f));
            f[0] = ids[*kind == 1 ? 0 : 1][i];
            if (HidD_GetFeature(found, f, flen)) break;
        }
    }
    return found;
}

static DWORD WINAPI gyro_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        if (!g_vk.gyro_on) { Sleep(500); continue; }
        int kind = 0;
        const char* name = "";
        HANDLE h = open_pad(&kind, &name);
        if (h == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }
        g_name = name;
        g_found = 1;
        printf("[gyro] %s found: its gyro aims (settings \"Motion controller\")\n", name);
        // the gyro's resting offset: measured while the pad lies still (first half second, then whenever
        // it's still for a second -- slowly, so a slow deliberate turn isn't taken for drift)
        float bx = 0, by = 0, bz = 0, sx = 0, sy = 0, sz = 0;
        int still_n = 0, have_bias = 0;
        LARGE_INTEGER fq, last, now;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&last);
        BYTE r[1024];   // (a DualShock 4 over Bluetooth reports in 547-byte pieces: the read must take a whole one)
        DWORD n;
        while (g_vk.gyro_on && ReadFile(h, r, sizeof(r), &n, NULL)) {
            float gx, gy, gz;
            if (!parse(kind, r, n, &gx, &gy, &gz)) continue;
            QueryPerformanceCounter(&now);
            float dt = (float)(now.QuadPart - last.QuadPart) / (float)fq.QuadPart;
            last = now;
            if (dt > 0.05f) dt = 0.05f;
            splat_gyro_reports++;
            float rx = gx - bx, ry = gy - by, rz = gz - bz;
            if (fabsf(rx) < 3.0f && fabsf(ry) < 3.0f && fabsf(rz) < 3.0f) {
                sx += gx; sy += gy; sz += gz;
                if (++still_n >= (have_bias ? 250 : 120)) {   // (~1 s / ~0.5 s of reports)
                    float k = have_bias ? 0.2f : 1.0f;
                    bx += (sx / still_n - bx) * k; by += (sy / still_n - by) * k; bz += (sz / still_n - bz) * k;
                    have_bias = 1;
                    sx = sy = sz = 0; still_n = 0;
                }
            } else { sx = sy = sz = 0; still_n = 0; }
            if (!have_bias) continue;
            // (a tiny dead band: a resting pad's noise doesn't creep the aim)
            if (fabsf(rx) < 0.4f) rx = 0;
            if (fabsf(ry) < 0.4f) ry = 0;
            EnterCriticalSection(&g_cs);
            g_acc_pitch += rx * dt;
            g_acc_yaw += ry * dt;
            LeaveCriticalSection(&g_cs);
        }
        CloseHandle(h);
        g_found = 0;
        printf("[gyro] %s gone (unplugged, or the setting turned off)\n", name);
        Sleep(1000);
    }
    return 0;
}

void gyro_start(void) {
    static int started;
    if (started) return;
    started = 1;
    InitializeCriticalSection(&g_cs);
    const char* e = getenv("SPLAT_GYRO");
    if (e) g_vk.gyro_on = atoi(e) != 0;
    HANDLE t = CreateThread(NULL, 64 * 1024, gyro_thread, NULL, 0, NULL);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(t); }
}

// Degrees turned since the last call (pitch: up +, yaw: left +); 0 = no pad.
int gyro_take(float* dpitch, float* dyaw) {
    if (!g_found) { *dpitch = *dyaw = 0; return 0; }
    EnterCriticalSection(&g_cs);
    *dpitch = g_acc_pitch; *dyaw = g_acc_yaw;
    g_acc_pitch = g_acc_yaw = 0;
    LeaveCriticalSection(&g_cs);
    return 1;
}
const char* gyro_name(void) { return g_found ? g_name : NULL; }
#else
void gyro_start(void) {}
int gyro_take(float* dpitch, float* dyaw) { *dpitch = *dyaw = 0; return 0; }
const char* gyro_name(void) { return NULL; }
#endif
