// NSR -- Native Splat Recomp Loader: the launcher / installer / mod hub built
// into the game program itself (like Unleashed Recomp: one NSR.exe, the
// player's own dump or disc image, nothing to compile).
//
// The window is WebView2 (the Edge engine built into Windows, GPU drawn)
// showing launcher\ui\nsr_loader_ui.html. The page sends commands with
// chrome.webview.postMessage("browse_dump" / "main" / "mod:3:1" ...); this
// file answers with one JSON state object (push_state) that the page draws.
// The install runs on its own thread (install_thread) and only posts lines
// back, so the window never waits for it.
//
// Install: 1/3 check the game (dump: the title folders; image: read the base
// game out of the .wux/.wud with the player's own keys.txt into
// <install>\gamedata\base + a junction to their update), 2/3 check the
// update's Gambit.rpx (SHA-256 of v272) and patch it (rpx_patch) into
// <install>\data, 3/3 install.ini + NSR.exe + ui\ copied into the install
// folder. Play: the window closes (music stops with it) and nsr_wizard()
// returns the paths; the game runs in this same process.
//
// Mods page: folders in <install>\mods (Cemu graphic packs with rules.txt, or
// any folder with a content\ folder). Enabled mods' content\ folders are
// passed to the game as SPLAT_MOD_DIRS; fs.c checks them before the game's
// own files.
//
// Compiled into recomp_runner through runtime/nsr_wizard.c; the disc reader
// and rpx_patch come in through runtime/nsr_wud_lib.c and nsr_rpx_lib.c with
// their printf routed here (wiz_printf).
//
// Nothing is written outside the install folder except settings.ini and the
// WebView2 cache (webview\) next to NSR.exe. No network. The dump is only read.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <winioctl.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <zlib.h>   // (mods added as .zip files: inflate)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "nsr_theme.h"
#include "third_party/webview2/WebView2.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

int nsr_wud_main(int argc, char** argv);     // launcher/nsr_wud.c  (runtime/nsr_wud_lib.c)
int rpx_patch_main(int argc, char** argv);   // tools/rpx_patch.c   (runtime/nsr_rpx_lib.c)
extern volatile int nsr_wud_cancel;          // (nsr_wud.c: stops an image extract)
int wiz_printf(const char* fmt, ...);
int wiz_fprintf(FILE* f, const char* fmt, ...);
#define RPX_V272_SHA256 "b81d1b872efd6b8a3d9210737751ef288a40535f6fa6f79e83e45a0c4ee10ac0"   // the update's code\Gambit.rpx this build was made from

// Symbols in texts sent to the page (this file is plain ANSI; the JSON writer turns these bytes into \uXXXX)
#define CHECK "\x01"   // check mark
#define DOT   "\x02"   // middle dot
#define ELL   "\x03"   // ellipsis

// ---- state --------------------------------------------------------------------------------------
enum { LINK_UPDATE = 1, LINK_KEYS = 2 };
static struct {
    HWND hwnd;
    char exe_dir[MAX_PATH], ui_dir[MAX_PATH];
    char dump[MAX_PATH], upd[MAX_PATH], base[MAX_PATH];   // the dump (folder or image), and the update / base title folders
    bool dump_is_image;
    char update_dir[MAX_PATH];                            // with an image: where the v272 update dump is
    char keys_file[MAX_PATH];                             // optional: the player's keys.txt (else next to the image / Cemu's)
    bool dump_ok; char dump_msg[512]; int dump_links;
    char inst[MAX_PATH]; bool inst_ok; char inst_msg[512]; double inst_free_gb, inst_need_gb;
    bool fullscreen, show_fps, music_on, reduce_motion, perf_mode;
    bool bench;   // (this start only: the built-in benchmark, every map)
    int volume;   // 0..100
    // installing (a thread in this process)
    HANDLE worker; volatile bool building; bool built, failed, cancelling;
    int bstep; char bstep_text[160], detail[160]; double progress;   // 0..1
    ULONGLONG build_t0; char fail_msg[512];
    double size_gb;
    // the page's frame rate while installing (smoothness check)
    int fps_n, fps_min, fps_sum, gap_max;
    // playing
    bool play_now;
    char out_rpx[MAX_PATH], out_game[MAX_PATH], out_saves[MAX_PATH];
    // WebView2
    ICoreWebView2Environment* env; ICoreWebView2Controller* ctrl; ICoreWebView2* wv;
    bool ui_ready, dirty, vol_sent;
    int tab;   // 0 home, 1 mods
    bool auto_test;   // NSR_TEST_AUTO=1: install and Play by themselves (unattended tests)
    int splash_tick;
} S;

// ---- settings.ini (next to NSR.exe; copied into the install folder once there is one) ----------
static void settings_path(char* out, size_t n, const char* dir) { snprintf(out, n, "%s\\settings.ini", dir); }
static void settings_load(void) {
    char p[MAX_PATH]; settings_path(p, sizeof(p), S.exe_dir);
    GetPrivateProfileStringA("nsr", "dump", "", S.dump, MAX_PATH, p);
    GetPrivateProfileStringA("nsr", "install", "", S.inst, MAX_PATH, p);
    GetPrivateProfileStringA("nsr", "update", "", S.update_dir, MAX_PATH, p);
    GetPrivateProfileStringA("nsr", "keys", "", S.keys_file, MAX_PATH, p);
    S.fullscreen = GetPrivateProfileIntA("nsr", "fullscreen", 0, p) != 0;
    S.show_fps = GetPrivateProfileIntA("nsr", "fps_counter", 1, p) != 0;
    S.perf_mode = GetPrivateProfileIntA("nsr", "performance_mode", 0, p) != 0;
    S.music_on = GetPrivateProfileIntA("nsr", "music", 1, p) != 0;
    S.reduce_motion = GetPrivateProfileIntA("nsr", "reduce_motion", 0, p) != 0;
    S.volume = GetPrivateProfileIntA("nsr", "volume", 60, p);
    if (S.volume < 0) S.volume = 0;
    if (S.volume > 100) S.volume = 100;
}
static void settings_save_to(const char* dir) {
    char p[MAX_PATH], v[32]; settings_path(p, sizeof(p), dir);
    WritePrivateProfileStringA("nsr", "dump", S.dump, p);
    WritePrivateProfileStringA("nsr", "install", S.inst, p);
    WritePrivateProfileStringA("nsr", "update", S.update_dir, p);
    WritePrivateProfileStringA("nsr", "keys", S.keys_file, p);
    #define WI(k, val) do { snprintf(v, sizeof(v), "%d", (int)(val)); WritePrivateProfileStringA("nsr", k, v, p); } while (0)
    WI("fullscreen", S.fullscreen); WI("fps_counter", S.show_fps); WI("performance_mode", S.perf_mode); WI("music", S.music_on); WI("reduce_motion", S.reduce_motion);
    WI("volume", S.volume);
    #undef WI
}
static bool is_dir(const char* p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static bool exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static void settings_save(void) {
    settings_save_to(S.exe_dir);
    if (S.inst[0] && is_dir(S.inst)) settings_save_to(S.inst);
}

// ---- the launcher's own log (webview\launcher_log.txt next to NSR.exe): window / WebView2 steps --------
static void lg(const char* fmt, ...) {
    char p[MAX_PATH + 48];
    snprintf(p, sizeof(p), "%s\\webview\\launcher_log.txt", S.exe_dir);
    FILE* f = fopen(p, "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc(10, f);
    fclose(f);
}

// ---- SHA-256 of a file (Windows' own CryptoAPI), as 64 hex characters --------------------------
static bool sha256_file(const char* path, char out[65]) {
    HCRYPTPROV prov = 0; HCRYPTHASH h = 0;
    out[0] = 0;
    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &h)) { CryptReleaseContext(prov, 0); return false; }
    FILE* f = fopen(path, "rb");
    if (!f) { CryptDestroyHash(h); CryptReleaseContext(prov, 0); return false; }
    unsigned char* buf = (unsigned char*)malloc(1 << 16);
    size_t n;
    while ((n = fread(buf, 1, 1 << 16, f)) > 0) CryptHashData(h, buf, (DWORD)n, 0);
    free(buf);
    fclose(f);
    BYTE d[32]; DWORD dl = 32;
    bool ok = CryptGetHashParam(h, HP_HASHVAL, d, &dl, 0) != 0;
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
    CryptDestroyHash(h); CryptReleaseContext(prov, 0);
    return ok;
}

// ---- the dump --------------------------------------------------------------------------------------
static bool file_has(const char* path, const char* needle) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char* buf = (char*)malloc(1 << 20);
    size_t n = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    buf[n] = 0;
    bool ok = strstr(buf, needle) != NULL;
    free(buf);
    return ok;
}
static bool is_title_dir(const char* d, bool* is_update) {
    char p[MAX_PATH + 32], q[MAX_PATH + 32];
    snprintf(p, sizeof(p), "%s\\meta\\meta.xml", d);
    if (!exists(p)) return false;
    snprintf(q, sizeof(q), "%s\\code\\Gambit.rpx", d);
    if (!exists(q)) return false;
    if (!file_has(p, ">0005000010176900</title_id>")) return false;
    *is_update = file_has(p, ">272</title_version>");
    return true;
}
static void scan_dump_dir(const char* d, int depth) {
    bool upd;
    if (is_title_dir(d, &upd)) { if (upd) snprintf(S.upd, MAX_PATH, "%s", d); else snprintf(S.base, MAX_PATH, "%s", d); return; }
    if (depth >= 4) return;
    char pat[MAX_PATH + 8];
    snprintf(pat, sizeof(pat), "%s\\*", d);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        char sub[MAX_PATH];
        snprintf(sub, sizeof(sub), "%s\\%s", d, fd.cFileName);
        scan_dump_dir(sub, depth + 1);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static bool is_image_path(const char* p) { size_t n = strlen(p); return n > 4 && (!_stricmp(p + n - 4, ".wux") || !_stricmp(p + n - 4, ".wud")); }
static bool rpx_is_v272(const char* upd) {   // (remembered per path: the hash takes ~50 ms)
    static char last[MAX_PATH + 32]; static bool last_ok;
    char p[MAX_PATH + 32], h[65];
    snprintf(p, sizeof(p), "%s\\code\\Gambit.rpx", upd);
    if (!_stricmp(p, last)) return last_ok;
    last_ok = sha256_file(p, h) && !strcmp(h, RPX_V272_SHA256);
    snprintf(last, sizeof(last), "%s", p);
    return last_ok;
}
// the keys.txt the disc reader will use (the one picked, next to the image, Cemu's), or false
static bool find_keys(char* out) {
    if (S.keys_file[0] && exists(S.keys_file)) { snprintf(out, MAX_PATH, "%s", S.keys_file); return true; }
    char k[MAX_PATH + 16];
    snprintf(k, sizeof(k), "%s", S.dump);
    char* q = strrchr(k, '\\');
    if (q) { strcpy(q + 1, "keys.txt"); if (exists(k)) { snprintf(out, MAX_PATH, "%s", k); return true; } }
    const char* env[2] = { "APPDATA", "LOCALAPPDATA" };
    for (int i = 0; i < 2; i++) {
        char e[MAX_PATH];
        if (GetEnvironmentVariableA(env[i], e, MAX_PATH)) { snprintf(k, sizeof(k), "%s\\Cemu\\keys.txt", e); if (exists(k)) { snprintf(out, MAX_PATH, "%s", k); return true; } }
    }
    return false;
}
static void check_dump(void) {
    S.dump_ok = false; S.upd[0] = S.base[0] = 0; S.dump_links = 0;
    S.dump_is_image = is_image_path(S.dump);
    if (!S.dump[0]) { snprintf(S.dump_msg, sizeof(S.dump_msg), "Pick your Dumpling dump folder (the game + its v272 update) or a .wux / .wud disc image"); return; }
    if (!exists(S.dump)) { snprintf(S.dump_msg, sizeof(S.dump_msg), "That %s isn't there any more", S.dump_is_image ? "file" : "folder"); return; }
    if (S.dump_is_image) {
        // the image holds the original game; the v272 update comes from a dump folder (looked for beside the image first)
        if (S.update_dir[0] && is_dir(S.update_dir)) scan_dump_dir(S.update_dir, 0);
        if (!S.upd[0]) {
            char d[MAX_PATH]; snprintf(d, sizeof(d), "%s", S.dump);
            char* q = strrchr(d, '\\');
            if (q) { *q = 0; scan_dump_dir(d, 1); if (S.upd[0]) snprintf(S.update_dir, MAX_PATH, "%s", d); }
        }
        S.base[0] = 0;   // (the image is the base game)
        if (!S.upd[0]) { snprintf(S.dump_msg, sizeof(S.dump_msg), "A disc image holds the original game only: NSR also needs your v272 update dump"); S.dump_links = LINK_UPDATE; return; }
        if (!rpx_is_v272(S.upd)) { snprintf(S.dump_msg, sizeof(S.dump_msg), "That update's Gambit.rpx isn't v272 (its SHA-256 differs)"); S.dump_links = LINK_UPDATE; return; }
        char k[MAX_PATH];
        if (!find_keys(k)) { snprintf(S.dump_msg, sizeof(S.dump_msg), "No keys.txt found: put your own keys.txt next to the image (one key per line). NSR has no keys"); S.dump_links = LINK_KEYS; return; }
        S.dump_ok = true;
        snprintf(S.dump_msg, sizeof(S.dump_msg), CHECK " v272 verified " DOT " keys.txt found " DOT " update found");
        return;
    }
    scan_dump_dir(S.dump, 0);
    if (!S.upd[0] && !S.base[0]) { snprintf(S.dump_msg, sizeof(S.dump_msg), "No Splatoon found here (looking for code\\, content\\ and meta\\meta.xml)"); return; }
    if (!S.upd[0]) { snprintf(S.dump_msg, sizeof(S.dump_msg), "Found the game but not its v272 update: dump the update too"); return; }
    if (!S.base[0]) { snprintf(S.dump_msg, sizeof(S.dump_msg), "Found the v272 update but not the base game (its content\\ folder is needed)"); return; }
    if (!rpx_is_v272(S.upd)) { snprintf(S.dump_msg, sizeof(S.dump_msg), "That update's Gambit.rpx isn't v272 (its SHA-256 differs)"); return; }
    S.dump_ok = true;
    snprintf(S.dump_msg, sizeof(S.dump_msg), CHECK " v272 verified " DOT " game + update found " DOT " only read, never changed");
}

// ---- the install location --------------------------------------------------------------------------
static ULONGLONG free_bytes(const char* path) {
    char root[MAX_PATH];
    snprintf(root, sizeof(root), "%.3s", path);   // "X:\"
    ULARGE_INTEGER fr;
    if (!GetDiskFreeSpaceExA(root, &fr, NULL, NULL)) return 0;
    return fr.QuadPart;
}
#define NEED_GB 2.0          // NSR.exe, the prepared game program, the shader cache and logs
#define NEED_IMAGE_GB 6.0    // with a disc image: the game read out of it (about 3.5 GB) on top
static void default_install(void) {
    DWORD drives = GetLogicalDrives();
    ULONGLONG best = 0; char best_drive = 0;
    for (int i = 0; i < 26; i++) {
        if (!(drives & (1u << i))) continue;
        char root[4] = { (char)('A' + i), ':', '\\', 0 };
        if (GetDriveTypeA(root) != DRIVE_FIXED) continue;
        ULONGLONG fr = free_bytes(root);
        if (root[0] == 'C') fr = fr > (8ull << 30) ? fr - (8ull << 30) : 0;   // (C: only if nothing else has room)
        if (fr > best) { best = fr; best_drive = root[0]; }
    }
    if (!best_drive) best_drive = 'C';
    snprintf(S.inst, MAX_PATH, "%c:\\NSR", best_drive);
}
static bool starts_with_i(const char* s, const char* prefix) { return prefix[0] && _strnicmp(s, prefix, strlen(prefix)) == 0; }
static bool dir_empty(const char* d) {
    char pat[MAX_PATH + 8];
    snprintf(pat, sizeof(pat), "%s\\*", d);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool empty = true;
    do { if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) { empty = false; break; } } while (FindNextFileA(h, &fd));
    FindClose(h);
    return empty;
}
static bool is_nsr_install(const char* d) {
    char p[MAX_PATH + 32];
    snprintf(p, sizeof(p), "%s\\data\\install.ini", d);
    return exists(p);
}
static void check_install_dir(void) {
    S.inst_ok = false; S.inst_free_gb = 0;
    S.inst_need_gb = S.dump_is_image ? NEED_IMAGE_GB : NEED_GB;
    if (!S.inst[0]) { snprintf(S.inst_msg, sizeof(S.inst_msg), "Pick a folder"); return; }
    if (strlen(S.inst) < 4 || S.inst[1] != ':' || S.inst[2] != '\\') { snprintf(S.inst_msg, sizeof(S.inst_msg), "Use a full path like D:\\NSR"); return; }
    char win[MAX_PATH] = "", pf[MAX_PATH] = "", pf86[MAX_PATH] = "", prof[MAX_PATH] = "";
    GetEnvironmentVariableA("WINDIR", win, MAX_PATH);
    GetEnvironmentVariableA("ProgramFiles", pf, MAX_PATH);
    GetEnvironmentVariableA("ProgramFiles(x86)", pf86, MAX_PATH);
    GetEnvironmentVariableA("USERPROFILE", prof, MAX_PATH);
    if (starts_with_i(S.inst, win) || starts_with_i(S.inst, pf) || starts_with_i(S.inst, pf86) || !_stricmp(S.inst, prof)) {
        snprintf(S.inst_msg, sizeof(S.inst_msg), "Not inside Windows, Program Files or the user folder itself"); return;
    }
    if (S.dump[0] && !S.dump_is_image && (starts_with_i(S.inst, S.dump) || starts_with_i(S.dump, S.inst))) { snprintf(S.inst_msg, sizeof(S.inst_msg), "Not inside the dump folder (or around it)"); return; }
    S.inst_free_gb = (double)free_bytes(S.inst) / (1024.0 * 1024 * 1024);
    bool mine = is_dir(S.inst) && is_nsr_install(S.inst);
    if (S.inst_free_gb < S.inst_need_gb && !mine) { snprintf(S.inst_msg, sizeof(S.inst_msg), "Not enough room on %.2s", S.inst); return; }
    S.inst_ok = true;
    S.inst_msg[0] = 0;
}
// installed = data\ has the prepared game program, made from this dump
static void check_built(void) {
    S.built = false;
    if (!S.inst[0]) return;
    char p[MAX_PATH + 48], d[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\data\\Gambit_v272_patched.rpx", S.inst);
    if (!exists(p)) return;
    snprintf(p, sizeof(p), "%s\\data\\install.ini", S.inst);
    GetPrivateProfileStringA("install", "dump", "", d, MAX_PATH, p);
    S.built = !S.dump[0] || !_stricmp(d, S.dump);
}

// ---- file pickers -------------------------------------------------------------------------------
static bool pick_path(const WCHAR* title, char* out, bool folder, const WCHAR* filter_name, const WCHAR* filter_spec) {
    bool ok = false;
    IFileOpenDialog* dlg = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&dlg))) {
        DWORD opts = 0;
        IFileOpenDialog_GetOptions(dlg, &opts);
        IFileOpenDialog_SetOptions(dlg, opts | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST) | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (!folder && filter_spec) { COMDLG_FILTERSPEC fs = { filter_name, filter_spec }; IFileOpenDialog_SetFileTypes(dlg, 1, &fs); }
        IFileOpenDialog_SetTitle(dlg, title);
        if (SUCCEEDED(IFileOpenDialog_Show(dlg, S.hwnd))) {
            IShellItem* item = NULL;
            if (SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
                PWSTR path = NULL;
                if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                    WideCharToMultiByte(CP_ACP, 0, path, -1, out, MAX_PATH, NULL, NULL);
                    CoTaskMemFree(path);
                    ok = true;
                }
                IShellItem_Release(item);
            }
        }
        IFileOpenDialog_Release(dlg);
    }
    return ok;
}
static bool pick_folder(const WCHAR* title, char* out) { return pick_path(title, out, true, NULL, NULL); }
// a small native menu at the mouse (Browse / Settings); returns the item number (1..) or 0
static int popup(const WCHAR* const* items, const bool* checked, const bool* enabled, int n) {
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < n; i++) {
        if (!items[i]) { AppendMenuW(m, MF_SEPARATOR, 0, NULL); continue; }
        UINT f = MF_STRING | (checked && checked[i] ? MF_CHECKED : 0) | (enabled && !enabled[i] ? MF_GRAYED : 0);
        AppendMenuW(m, f, (UINT_PTR)(i + 1), items[i]);
    }
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(S.hwnd);
    int r = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0, S.hwnd, NULL);
    DestroyMenu(m);
    return r;
}

// ---- small file helpers -------------------------------------------------------------------------
static void remove_tree(const char* d) {
    char buf[MAX_PATH + 2];
    snprintf(buf, sizeof(buf), "%s", d);
    buf[strlen(buf) + 1] = 0;   // double null
    SHFILEOPSTRUCTA op; memset(&op, 0, sizeof(op));
    op.wFunc = FO_DELETE; op.pFrom = buf; op.fFlags = FOF_NO_UI;
    SHFileOperationA(&op);
}
static bool copy_tree(const char* src, const char* dst) {
    CreateDirectoryA(dst, NULL);
    char pat[MAX_PATH + 8];
    snprintf(pat, sizeof(pat), "%s\\*", src);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char a[MAX_PATH], b[MAX_PATH];
        snprintf(a, sizeof(a), "%s\\%s", src, fd.cFileName);
        snprintf(b, sizeof(b), "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= copy_tree(a, b);
        else ok &= CopyFileA(a, b, FALSE) != 0;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}
static ULONGLONG dir_size(const char* d, int depth) {
    char pat[MAX_PATH + 8];
    snprintf(pat, sizeof(pat), "%s\\*", d);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ULONGLONG t = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // (gamedata\update links to the dump)
        char sub[MAX_PATH];
        snprintf(sub, sizeof(sub), "%s\\%s", d, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 8) t += dir_size(sub, depth + 1); }
        else t += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return t;
}
// A directory junction (like mklink /J): the install folder's gamedata\update points at the player's update dump
static bool make_junction(const char* link, const char* target) {
    CreateDirectoryA(link, NULL);
    HANDLE h = CreateFileA(link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    WCHAR wt[MAX_PATH + 8], wp[MAX_PATH + 8];
    wcscpy(wt, L"\\??\\");
    MultiByteToWideChar(CP_ACP, 0, target, -1, wt + 4, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, target, -1, wp, MAX_PATH);
    int tl = (int)wcslen(wt); while (tl > 5 && wt[tl - 1] == L'\\') wt[--tl] = 0;
    int pl = (int)wcslen(wp); while (pl > 1 && wp[pl - 1] == L'\\') wp[--pl] = 0;
    struct { DWORD tag; WORD len, reserved; WORD sn_off, sn_len, pn_off, pn_len; WCHAR buf[2 * MAX_PATH + 16]; } rd;
    memset(&rd, 0, sizeof(rd));
    rd.tag = IO_REPARSE_TAG_MOUNT_POINT;
    rd.sn_off = 0; rd.sn_len = (WORD)(tl * 2); memcpy(rd.buf, wt, (tl + 1) * 2);
    rd.pn_off = (WORD)((tl + 1) * 2); rd.pn_len = (WORD)(pl * 2); memcpy(rd.buf + tl + 1, wp, (pl + 1) * 2);
    rd.len = (WORD)(8 + rd.sn_len + 2 + rd.pn_len + 2);
    DWORD ret;
    bool ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &rd, 8 + rd.len, NULL, 0, &ret, NULL) != 0;
    CloseHandle(h);
    return ok;
}

// ---- the install (a thread in this process) ------------------------------------------------------
#define WM_BUILD_LINE (WM_APP + 1)
#define WM_BUILD_EXIT (WM_APP + 2)
#define WM_UI_CMD     (WM_APP + 3)   // a command from the page (handled outside WebView2's callback)
#define WM_DO_PLAY    (WM_APP + 4)
static FILE* g_install_log;
// The tools' printf: each line goes to logs\install.txt and to the window (progress, the last line)
static void wiz_vprint(const char* fmt, va_list ap) {
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    static char line[2048]; static int ln;   // (one writer: the install thread)
    for (char* c = buf; *c; c++) {
        if (*c == '\r') continue;
        if (*c == '\n' || ln >= (int)sizeof(line) - 1) {
            line[ln] = 0;
            if (g_install_log) { fprintf(g_install_log, "%s\n", line); fflush(g_install_log); }
            if (S.hwnd) PostMessageA(S.hwnd, WM_BUILD_LINE, 0, (LPARAM)_strdup(line));
            ln = 0;
            if (*c != '\n') line[ln++] = *c;
        } else line[ln++] = *c;
    }
}
int wiz_printf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); wiz_vprint(fmt, ap); va_end(ap); return 0; }
int wiz_fprintf(FILE* f, const char* fmt, ...) { (void)f; va_list ap; va_start(ap, fmt); wiz_vprint(fmt, ap); va_end(ap); return 0; }

static DWORD WINAPI install_thread(LPVOID arg) {
    (void)arg;
    char p[MAX_PATH + 64], q[MAX_PATH + 64], gamedir[MAX_PATH];
    static const char* parts[] = { "game", "game\\cache", "data", "saves", "logs", "mods" };
    CreateDirectoryA(S.inst, NULL);
    for (int i = 0; i < 6; i++) { snprintf(p, sizeof(p), "%s\\%s", S.inst, parts[i]); CreateDirectoryA(p, NULL); }
    snprintf(p, sizeof(p), "%s\\logs\\install.txt", S.inst);
    g_install_log = fopen(p, "w");
    wiz_printf("NSR-STEP 1/3 Checking the game\n");
    if (S.dump_is_image) {
        snprintf(gamedir, sizeof(gamedir), "%s\\gamedata", S.inst);
        CreateDirectoryA(gamedir, NULL);
        snprintf(p, sizeof(p), "%s\\gamedata\\base", S.inst);
        wiz_printf("  reading the game out of the disc image with your keys.txt (the image is only read)\n");
        const char* av[6] = { "nsr_wud", "extract", S.dump, p, S.keys_file, NULL };
        if (nsr_wud_main(S.keys_file[0] ? 5 : 4, (char**)av) != 0) {
            wiz_printf(nsr_wud_cancel ? "NSR-FAIL cancelled\n" : "NSR-FAIL the disc image couldn't be read (usually the key: see logs\\install.txt)\n");
            goto fail;
        }
        snprintf(q, sizeof(q), "%s\\gamedata\\update", S.inst);
        RemoveDirectoryA(q);   // (an earlier link; this removes the link only, never the dump)
        if (!make_junction(q, S.upd)) { wiz_printf("NSR-FAIL couldn't link the update folder into gamedata\n"); goto fail; }
        wiz_printf("  update: %s (linked as gamedata\\update)\n", S.upd);
    } else {
        snprintf(gamedir, sizeof(gamedir), "%s", S.dump);
        wiz_printf("  update: %s\n  base:   %s\n", S.upd, S.base);
    }
    {   // NSR_TEST_LOAD=seconds (tests only): read the base game's files like a disc-image extract would, with the
        // same per-file progress lines, to check the window stays smooth during a long install
        char e[16];
        if (GetEnvironmentVariableA("NSR_TEST_LOAD", e, sizeof(e)) && S.base[0]) {
            ULONGLONG until = GetTickCount64() + (ULONGLONG)atoi(e) * 1000;
            unsigned n = 0;
            char pat[MAX_PATH + 32], path[MAX_PATH * 2];
            snprintf(pat, sizeof(pat), "%s\\content\\Model\\*", S.base);
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(pat, &fd);
            unsigned char* buf = (unsigned char*)malloc(1 << 20);
            while (h != INVALID_HANDLE_VALUE && GetTickCount64() < until && !nsr_wud_cancel) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    snprintf(path, sizeof(path), "%s\\content\\Model\\%s", S.base, fd.cFileName);
                    FILE* f = fopen(path, "rb");
                    if (f) { while (fread(buf, 1, 1 << 20, f) > 0) {} fclose(f); }
                    n++;
                    double pct = 100.0 * (double)(GetTickCount64() + (ULONGLONG)atoi(e) * 1000 - until) / ((double)atoi(e) * 1000);
                    wiz_printf("NSR-WUD %u/%u %.0f%% %s\n", n, 3000u, pct, fd.cFileName);
                }
                if (!FindNextFileA(h, &fd)) { FindClose(h); h = FindFirstFileA(pat, &fd); }
            }
            if (h != INVALID_HANDLE_VALUE) FindClose(h);
            free(buf);
            wiz_printf("  (test load: read %u files)\n", n);
        }
    }
    if (nsr_wud_cancel) { wiz_printf("NSR-FAIL cancelled\n"); goto fail; }
    wiz_printf("NSR-STEP 2/3 Preparing the game program\n");
    snprintf(p, sizeof(p), "%s\\code\\Gambit.rpx", S.upd);
    {
        char hash[65];
        if (!sha256_file(p, hash)) { wiz_printf("NSR-FAIL couldn't read %s\n", p); goto fail; }
        if (strcmp(hash, RPX_V272_SHA256) != 0) { wiz_printf("NSR-FAIL the update's Gambit.rpx isn't the v272 program this build was made from (sha256 %s)\n", hash); goto fail; }
        wiz_printf("  Gambit.rpx is the v272 program (sha256 ok)\n");
    }
    snprintf(q, sizeof(q), "%s\\data\\Gambit_v272_patched.rpx", S.inst);
    {
        const char* av[3] = { "rpx_patch", p, q };
        if (rpx_patch_main(3, (char**)av) != 0) { wiz_printf("NSR-FAIL preparing the game program failed\n"); goto fail; }
    }
    wiz_printf("NSR-STEP 3/3 Installing\n");
    {   // NSR itself and its window's files go into the install folder (start it from there next time)
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        snprintf(q, sizeof(q), "%s\\NSR.exe", S.inst);
        if (_stricmp(exe, q) != 0) { wiz_printf("  copying NSR.exe into the install folder\n"); if (!CopyFileA(exe, q, FALSE)) wiz_printf("  (couldn't copy NSR.exe there; keep starting it from where it is)\n"); }
        snprintf(q, sizeof(q), "%s\\ui", S.inst);
        if (S.ui_dir[0] && _stricmp(S.ui_dir, q) != 0) { wiz_printf("  copying the launcher's page (ui\\)\n"); copy_tree(S.ui_dir, q); }
    }
    snprintf(p, sizeof(p), "%s\\data\\install.ini", S.inst);
    {
        FILE* f = fopen(p, "w");
        if (!f) { wiz_printf("NSR-FAIL can't write %s\n", p); goto fail; }
        fprintf(f, "[install]\ndump=%s\ngame=%s\nupdate=%s\nimage=%d\n", S.dump, gamedir, S.upd, S.dump_is_image ? 1 : 0);
        fclose(f);
    }
    wiz_printf("NSR-DONE\n");
    if (g_install_log) { fclose(g_install_log); g_install_log = NULL; }
    PostMessageA(S.hwnd, WM_BUILD_EXIT, 0, 0);
    return 0;
fail:
    if (g_install_log) { fclose(g_install_log); g_install_log = NULL; }
    PostMessageA(S.hwnd, WM_BUILD_EXIT, 1, 0);
    return 0;
}
static void push_state(void);
static void layout(void);
static void build_start(void) {
    if (S.building || !S.dump_ok || !S.inst_ok) return;
    S.building = true; S.built = false; S.failed = false; S.cancelling = false; S.progress = 0; S.bstep = 0; S.fail_msg[0] = 0; S.detail[0] = 0;
    S.fps_n = 0; S.fps_min = 1000; S.fps_sum = 0; S.gap_max = 0;
    nsr_wud_cancel = 0;
    snprintf(S.bstep_text, sizeof(S.bstep_text), "Starting");
    S.build_t0 = GetTickCount64();
    settings_save();
    S.worker = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
    push_state();
}
static void build_cancel(void) {
    if (!S.building) return;
    S.cancelling = true;
    nsr_wud_cancel = 1;   // (the disc reader stops at its next 1 MB; the other steps take a second)
    push_state();
}
static void build_line(const char* s) {
    unsigned n, m; double pct;
    if (!strncmp(s, "NSR-STEP ", 9) && sscanf(s + 9, "%u/%u", &n, &m) == 2) {
        S.bstep = (int)n;
        const char* t = strchr(s + 9, ' ');
        snprintf(S.bstep_text, sizeof(S.bstep_text), "%s", t ? t + 1 : "");
        static const double at[4] = { 0, 0.02, 0.92, 0.97 };   // (reading a disc image is nearly all of it)
        S.progress = at[n < 3 ? n : 3];
        S.detail[0] = 0;
    } else if (!strncmp(s, "NSR-WUD ", 8) && sscanf(s + 8, "%u/%u %lf%%", &n, &m, &pct) == 3) {
        S.progress = 0.02 + 0.88 * pct / 100.0;
        snprintf(S.detail, sizeof(S.detail), "%u / %u files", n, m);
    } else if (!strncmp(s, "NSR-FAIL", 8)) {
        snprintf(S.fail_msg, sizeof(S.fail_msg), "%s", s + 9);
    } else if (!strncmp(s, "  copying NSR.exe", 17)) {
        snprintf(S.detail, sizeof(S.detail), "copying NSR.exe");
    }
    S.dirty = true;
}
static void build_exit(DWORD code) {
    S.building = false;
    if (S.worker) { WaitForSingleObject(S.worker, 2000); CloseHandle(S.worker); S.worker = NULL; }
    if (code == 0) {
        S.built = true; S.progress = 1.0;
        S.size_gb = dir_size(S.inst, 0) / (1024.0 * 1024 * 1024);
        settings_save();
    } else {
        S.failed = true;
        if (!S.fail_msg[0]) snprintf(S.fail_msg, sizeof(S.fail_msg), "the install stopped (code %lu)", (unsigned long)code);
    }
    // the page's smoothness while installing goes into logs\install.txt
    if (S.fps_n) {
        char p[MAX_PATH + 32];
        snprintf(p, sizeof(p), "%s\\logs\\install.txt", S.inst);
        FILE* f = fopen(p, "a");
        if (f) {
            fprintf(f, "launcher window while installing: %d half-seconds measured, lowest %d fps, average %.1f fps, longest frame %d ms, install took %.1f s\n",
                    S.fps_n, S.fps_min, (double)S.fps_sum / S.fps_n, S.gap_max, (GetTickCount64() - S.build_t0) / 1000.0);
            fclose(f);
        }
    }
    push_state();
}

// ---- mods (<install>\mods: Cemu graphic packs or folders with content\) -------------------------
typedef struct { char folder[MAX_PATH]; char sub[MAX_PATH]; char name[160]; char desc[400]; char status[220]; bool on, ok; } Mod;
// (sub: a mod zipped with one wrapper folder around it -- "MyMod\MyMod\content" -- is used from that inner folder)
static bool one_subdir_with_mod(const char* dir, char* out, size_t n) {
    char pat[MAX_PATH + 8], p[MAX_PATH * 2];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    int dirs = 0, files = 0;
    char only[MAX_PATH] = "";
    do {
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dirs++; snprintf(only, sizeof(only), "%s", fd.cFileName); }
        else if (_stricmp(fd.cFileName, "readme.txt") && _stricmp(fd.cFileName, "readme.md") && _stricmp(fd.cFileName, "desktop.ini")) files++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (dirs != 1 || files) return false;
    snprintf(p, sizeof(p), "%s\\%s\\content", dir, only);
    bool ok = is_dir(p);
    if (!ok) { snprintf(p, sizeof(p), "%s\\%s\\rules.txt", dir, only); ok = exists(p); }
    if (ok) snprintf(out, n, "%s", only);
    return ok;
}
// A .zip picked as a mod: read out into dst (stored and deflated entries; names with ".." or a drive are refused).
// Returns the number of files written, -1 if it isn't a zip.
static uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static void mkdirs_for(const char* path) {   // (every folder above the file)
    char tmp[MAX_PATH * 2];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* c = tmp + 3; *c; c++) if (*c == '\\') { *c = 0; CreateDirectoryA(tmp, NULL); *c = '\\'; }
}
static int zip_extract(const char* zip, const char* dst) {
    FILE* f = fopen(zip, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long tail = size < 66000 ? size : 66000;
    unsigned char* t = (unsigned char*)malloc((size_t)tail);
    fseek(f, size - tail, SEEK_SET);
    if (fread(t, 1, (size_t)tail, f) != (size_t)tail) { free(t); fclose(f); return -1; }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--) if (rd32(t + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(t); fclose(f); return -1; }
    uint32_t n = rd16(t + eocd + 10), cd_size = rd32(t + eocd + 12), cd_off = rd32(t + eocd + 16);
    free(t);
    unsigned char* cd = (unsigned char*)malloc(cd_size + 1);
    fseek(f, (long)cd_off, SEEK_SET);
    if (fread(cd, 1, cd_size, f) != cd_size) { free(cd); fclose(f); return -1; }
    int written = 0;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < n && pos + 46 <= cd_size; i++) {
        if (rd32(cd + pos) != 0x02014b50) break;
        uint32_t method = rd16(cd + pos + 10), csize = rd32(cd + pos + 20), usize = rd32(cd + pos + 24);
        uint32_t nlen = rd16(cd + pos + 28), xlen = rd16(cd + pos + 30), clen = rd16(cd + pos + 32), loff = rd32(cd + pos + 42);
        char name[MAX_PATH * 2];
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, cd + pos + 46, nlen); name[nlen] = 0;
        pos += 46 + rd16(cd + pos + 28) + xlen + clen;
        for (char* c = name; *c; c++) if (*c == '/') *c = '\\';
        bool bad = strstr(name, "..") || name[0] == '\\' || strchr(name, ':') || !name[0];
        if (bad || usize > (512u << 20) || (method != 0 && method != 8)) continue;
        if (name[strlen(name) - 1] == '\\') continue;   // (a folder entry: made with its files)
        // the local header holds its own name/extra lengths
        unsigned char lh[30];
        fseek(f, (long)loff, SEEK_SET);
        if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) continue;
        long data = (long)loff + 30 + rd16(lh + 26) + rd16(lh + 28);
        char out[MAX_PATH * 2];
        snprintf(out, sizeof(out), "%s\\%s", dst, name);
        mkdirs_for(out);
        unsigned char* cbuf = (unsigned char*)malloc(csize ? csize : 1);
        fseek(f, data, SEEK_SET);
        if (fread(cbuf, 1, csize, f) != csize) { free(cbuf); continue; }
        FILE* o = fopen(out, "wb");
        if (!o) { free(cbuf); continue; }
        if (method == 0) fwrite(cbuf, 1, csize, o);
        else {
            unsigned char* ubuf = (unsigned char*)malloc(usize ? usize : 1);
            z_stream zs; memset(&zs, 0, sizeof(zs));
            if (inflateInit2(&zs, -15) == Z_OK) {
                zs.next_in = cbuf; zs.avail_in = csize; zs.next_out = ubuf; zs.avail_out = usize;
                int r = inflate(&zs, Z_FINISH);
                if (r == Z_STREAM_END || r == Z_OK || r == Z_BUF_ERROR) fwrite(ubuf, 1, zs.total_out, o);
                inflateEnd(&zs);
            }
            free(ubuf);
        }
        fclose(o);
        free(cbuf);
        written++;
    }
    free(cd);
    fclose(f);
    return written;
}
static Mod g_mods[128];
static int g_nmods;
static char g_mods_msg[400], g_mods_empty[400];
static bool g_mods_warn;
static bool any_file(const char* dir, const char* pattern) {
    char pat[MAX_PATH + 64];
    snprintf(pat, sizeof(pat), "%s\\%s", dir, pattern);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}
static void trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    size_t i = 0; while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, n - i + 1);
    if (s[0] == '"') { memmove(s, s + 1, strlen(s)); n = strlen(s); if (n && s[n - 1] == '"') s[n - 1] = 0; }
}
static int mod_cmp(const void* a, const void* b) { return _stricmp(((const Mod*)a)->name, ((const Mod*)b)->name); }
static void mods_scan(void) {
    g_nmods = 0; g_mods_msg[0] = 0; g_mods_empty[0] = 0; g_mods_warn = false;
    if (!S.inst[0] || !is_nsr_install(S.inst)) {
        snprintf(g_mods_msg, sizeof(g_mods_msg), "Install the game first: mods go into the install folder's mods\\ folder");
        g_mods_warn = true;
        return;
    }
    char md[MAX_PATH + 8], ini[MAX_PATH + 16], pat[MAX_PATH + 16];
    snprintf(md, sizeof(md), "%s\\mods", S.inst);
    CreateDirectoryA(md, NULL);
    snprintf(ini, sizeof(ini), "%s\\settings.ini", S.inst);
    snprintf(pat, sizeof(pat), "%s\\*", md);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || g_nmods >= 128) continue;
            Mod* m = &g_mods[g_nmods++];
            memset(m, 0, sizeof(*m));
            snprintf(m->folder, sizeof(m->folder), "%s", fd.cFileName);
            snprintf(m->name, sizeof(m->name), "%s", fd.cFileName);
            char dir[MAX_PATH * 2], p[MAX_PATH * 2], line[1024], ids[512] = "";
            snprintf(dir, sizeof(dir), "%s\\%s", md, fd.cFileName);
            snprintf(p, sizeof(p), "%s\\content", dir);
            if (!is_dir(p)) {
                snprintf(p, sizeof(p), "%s\\rules.txt", dir);
                if (!exists(p) && one_subdir_with_mod(dir, m->sub, sizeof(m->sub))) snprintf(dir, sizeof(dir), "%s\\%s\\%s", md, fd.cFileName, m->sub);
            }
            snprintf(p, sizeof(p), "%s\\rules.txt", dir);
            bool has_rules = false, texture_rules = false;
            FILE* f = fopen(p, "rb");
            if (f) {   // Cemu's rules.txt: [Definition] name / description / titleIds
                has_rules = true;
                bool def = false;
                while (fgets(line, sizeof(line), f)) {
                    trim(line);
                    if (line[0] == '[') { def = !_strnicmp(line, "[Definition]", 12); if (!_strnicmp(line, "[TextureRedefine]", 17)) texture_rules = true; continue; }
                    if (!def || line[0] == '#') continue;
                    char* eq = strchr(line, '=');
                    if (!eq) continue;
                    *eq = 0;
                    char key[64]; snprintf(key, sizeof(key), "%s", line); trim(key);
                    char* val = eq + 1; trim(val);
                    if (!_stricmp(key, "name")) snprintf(m->name, sizeof(m->name), "%s", val);
                    else if (!_stricmp(key, "description")) snprintf(m->desc, sizeof(m->desc), "%s", val);
                    else if (!_stricmp(key, "titleIds")) snprintf(ids, sizeof(ids), "%s", val);
                }
                fclose(f);
            }
            for (char* c = m->desc; *c; c++) if (c[0] == '\\' && c[1] == 'n') { c[0] = ' '; c[1] = ' '; }
            snprintf(p, sizeof(p), "%s\\content", dir);
            bool content = is_dir(p);
            bool code = any_file(dir, "patch*.asm") || any_file(dir, "patches.txt");
            bool shaders = any_file(dir, "*_ps.txt") || any_file(dir, "*_vs.txt") || any_file(dir, "*_gs.txt");
            _strlwr(ids);
            bool other_game = ids[0] && !strstr(ids, "10176900") && !strstr(ids, "10176a00") && !strstr(ids, "10162b00");
            if (other_game) { m->ok = false; snprintf(m->status, sizeof(m->status), "Made for another game (its titleIds don't include Splatoon)"); }
            else if (content) {
                m->ok = true;
                snprintf(m->status, sizeof(m->status), "Replaces game files (content\\)%s", code || shaders || texture_rules ? " " DOT " its code patches / shader or resolution parts aren't used yet" : "");
            } else if (code || shaders || texture_rules) { m->ok = false; snprintf(m->status, sizeof(m->status), "Code patches / shader / resolution packs aren't supported yet (use F10 in the game for resolution)"); }
            else { m->ok = false; snprintf(m->status, sizeof(m->status), has_rules ? "Nothing to load: no content\\ folder" : "Not a mod: no rules.txt or content\\ folder"); }
            m->on = m->ok && GetPrivateProfileIntA("mods", m->folder, 0, ini) != 0;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    qsort(g_mods, g_nmods, sizeof(Mod), mod_cmp);
    int on = 0; for (int i = 0; i < g_nmods; i++) on += g_mods[i].on;
    if (g_nmods) snprintf(g_mods_msg, sizeof(g_mods_msg), "%d mod%s " DOT " %d enabled " DOT " enabled mods' files are used instead of the game's (the first in the list wins)", g_nmods, g_nmods == 1 ? "" : "s", on);
    else snprintf(g_mods_empty, sizeof(g_mods_empty), "No mods yet. Press Add mod" ELL " and pick a mod folder or a mod .zip (a Cemu graphic pack with rules.txt, or anything with a content\\ folder), or drop them into the mods folder.");
}
static void mods_toggle(int i, int on) {
    if (i < 0 || i >= g_nmods) return;
    char ini[MAX_PATH + 16];
    snprintf(ini, sizeof(ini), "%s\\settings.ini", S.inst);
    WritePrivateProfileStringA("mods", g_mods[i].folder, on ? "1" : "0", ini);
    mods_scan();
}
static void mods_add(void) {
    if (!is_nsr_install(S.inst)) return;
    static const WCHAR* const items[] = { L"A mod folder (a Cemu graphic pack, or a folder with content\\)", L"A mod .zip file (as downloaded)" };
    int which = popup(items, NULL, NULL, 2);
    if (!which) return;
    char src[MAX_PATH] = "";
    if (which == 2) {
        if (!pick_path(L"Pick a mod .zip", src, false, L"Zip files", L"*.zip")) return;
        char nm[MAX_PATH];
        const char* base = strrchr(src, '\\');
        snprintf(nm, sizeof(nm), "%s", base ? base + 1 : src);
        size_t l = strlen(nm);
        if (l > 4 && !_stricmp(nm + l - 4, ".zip")) nm[l - 4] = 0;
        char dst[MAX_PATH * 2];
        snprintf(dst, sizeof(dst), "%s\\mods\\%s", S.inst, nm);
        if (exists(dst)) { MessageBoxA(S.hwnd, "A mod with that folder name is already there.", "NSR", MB_OK | MB_ICONINFORMATION); return; }
        CreateDirectoryA(dst, NULL);
        int nfiles = zip_extract(src, dst);
        if (nfiles <= 0) {
            remove_tree(dst);
            MessageBoxA(S.hwnd, nfiles < 0 ? "That file isn't a zip NSR can read (only .zip; not .rar or .7z)." : "Nothing usable in that zip.", "NSR", MB_OK | MB_ICONINFORMATION);
            return;
        }
        mods_scan();
        return;
    }
    if (!pick_folder(L"Pick a mod folder (a Cemu graphic pack or a folder with content\\)", src)) return;
    const char* name = strrchr(src, '\\');
    name = name ? name + 1 : src;
    char dst[MAX_PATH * 2];
    snprintf(dst, sizeof(dst), "%s\\mods\\%s", S.inst, name);
    if (exists(dst)) { MessageBoxA(S.hwnd, "A mod with that folder name is already there.", "NSR", MB_OK | MB_ICONINFORMATION); return; }
    // (Windows' own copy, with its progress window for big mods)
    char from[MAX_PATH + 2], to[MAX_PATH * 2 + 2];
    memset(from, 0, sizeof(from)); memset(to, 0, sizeof(to));
    snprintf(from, sizeof(from) - 1, "%s", src);
    snprintf(to, sizeof(to) - 1, "%s", dst);
    SHFILEOPSTRUCTA op; memset(&op, 0, sizeof(op));
    op.hwnd = S.hwnd; op.wFunc = FO_COPY; op.pFrom = from; op.pTo = to; op.fFlags = FOF_NOCONFIRMMKDIR | FOF_NOCONFIRMATION;
    SHFileOperationA(&op);
    mods_scan();
}
// the enabled mods' content folders for the game (SPLAT_MOD_DIRS, read by fs.c)
static void mods_env(char* out, size_t n) {
    out[0] = 0;
    mods_scan();
    size_t len = 0;
    for (int i = 0; i < g_nmods; i++) {
        if (!g_mods[i].on) continue;
        int w = g_mods[i].sub[0] ? snprintf(out + len, n - len, "%s%s\\mods\\%s\\%s\\content", len ? ";" : "", S.inst, g_mods[i].folder, g_mods[i].sub)
                                 : snprintf(out + len, n - len, "%s%s\\mods\\%s\\content", len ? ";" : "", S.inst, g_mods[i].folder);
        if (w < 0 || (size_t)w >= n - len) break;
        len += (size_t)w;
    }
}

// ---- remove ---------------------------------------------------------------------------------------
static void remove_install(void) {
    if (!S.inst[0] || !is_nsr_install(S.inst)) { MessageBoxA(S.hwnd, "There is no NSR install in that folder.", "NSR", MB_OK); return; }
    char q[800];
    snprintf(q, sizeof(q), "Remove the NSR installation?\n\n%s\n\nIts game\\, data\\, saves\\, logs\\, mods\\, gamedata\\ and ui\\ folders and settings.ini are deleted (saves too). Your dump and image are not touched.", S.inst);
    if (MessageBoxA(S.hwnd, q, "NSR", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
    static const char* parts[] = { "game", "data", "saves", "logs", "mods", "temp", "gamedata", "ui" };
    char p[MAX_PATH + 32];
    snprintf(p, sizeof(p), "%s\\gamedata\\update", S.inst); RemoveDirectoryA(p);   // (the link to the update dump: removed as a link only)
    for (int i = 0; i < 8; i++) { snprintf(p, sizeof(p), "%s\\%s", S.inst, parts[i]); if (exists(p)) remove_tree(p); }
    snprintf(p, sizeof(p), "%s\\settings.ini", S.inst); DeleteFileA(p);
    char me[MAX_PATH]; GetModuleFileNameA(NULL, me, MAX_PATH);
    snprintf(p, sizeof(p), "%s\\NSR.exe", S.inst);
    if (_stricmp(me, p) != 0) DeleteFileA(p);   // (not the copy that is running)
    RemoveDirectoryA(S.inst);   // (only goes if nothing else was in it)
    S.built = false;
    check_install_dir();
    mods_scan();
}

// ---- the page: one JSON state object ------------------------------------------------------------
typedef struct { char* b; size_t n, cap; } Js;
static void js_put(Js* j, const char* s, size_t l) {
    if (j->n + l + 1 > j->cap) { j->cap = (j->n + l + 1) * 2 + 256; j->b = (char*)realloc(j->b, j->cap); }
    memcpy(j->b + j->n, s, l); j->n += l; j->b[j->n] = 0;
}
static void js(Js* j, const char* s) { js_put(j, s, strlen(s)); }
// a JSON string from ANSI text: quotes/backslashes escaped, symbols (\x01..\x03) and other non-ASCII as \uXXXX
static void js_str(Js* j, const char* s) {
    js(j, "\"");
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        char t[16];
        if (*p == 1) js(j, "\\u2713");
        else if (*p == 2) js(j, "\\u00b7");
        else if (*p == 3) js(j, "\\u2026");
        else if (*p == '"') js(j, "\\\"");
        else if (*p == '\\') js(j, "\\\\");
        else if (*p < 0x20) { snprintf(t, sizeof(t), "\\u%04x", *p); js(j, t); }
        else if (*p < 0x80) js_put(j, (const char*)p, 1);
        else { WCHAR w = 0; MultiByteToWideChar(CP_ACP, 0, (const char*)p, 1, &w, 1); snprintf(t, sizeof(t), "\\u%04x", (unsigned)w); js(j, t); }
    }
    js(j, "\"");
}
static void js_kstr(Js* j, const char* k, const char* v) { js(j, "\""); js(j, k); js(j, "\":"); js_str(j, v); }
static void js_kbool(Js* j, const char* k, bool v) { js(j, "\""); js(j, k); js(j, "\":"); js(j, v ? "true" : "false"); }
static void js_knum(Js* j, const char* k, double v) { char t[64]; snprintf(t, sizeof(t), "\"%s\":%.2f", k, v); js(j, t); }
static void push_json(const char* s) {
    if (!S.wv || !S.ui_ready) return;
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    WCHAR* w = (WCHAR*)malloc(n * sizeof(WCHAR));
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    ICoreWebView2_PostWebMessageAsJson(S.wv, w);
    free(w);
}
static void push_state(void) {
    S.dirty = false;
    if (!S.wv || !S.ui_ready) return;
    Js j = { 0 };
    char t[600];
    js(&j, "{"); js_kbool(&j, "alpha", NSR_ALPHA_BANNER != 0); js(&j, ","); js_kstr(&j, "ver", NSR_VERSION);
    // step pills
    const char* st_t[4]; const char* st_c[4];
    st_t[0] = S.dump_ok ? CHECK " Game source" : "1 " DOT " Game source";
    st_c[0] = S.dump_ok ? "done" : "on";
    st_t[1] = S.inst_ok ? CHECK " Install location" : "2 " DOT " Install location";
    st_c[1] = S.inst_ok ? "done" : S.dump_ok ? "on" : "";
    st_t[2] = S.built ? CHECK " Install" : "3 " DOT " Install";
    st_c[2] = S.built ? "done" : (S.building || (S.dump_ok && S.inst_ok)) ? "on" : "";
    st_t[3] = "4 " DOT " Play";
    st_c[3] = S.built ? "on" : "";
    js(&j, ",\"steps\":[");
    for (int i = 0; i < 4; i++) { js(&j, i ? ",{" : "{"); js_kstr(&j, "t", st_t[i]); js(&j, ","); js_kstr(&j, "c", st_c[i]); js(&j, "}"); }
    js(&j, "]");
    // game source
    js(&j, ",\"dump\":{"); js_kstr(&j, "path", S.dump); js(&j, ","); js_kstr(&j, "msg", S.dump_msg); js(&j, ","); js_kbool(&j, "ok", S.dump_ok);
    js(&j, ",\"links\":[");
    bool first = true;
    if (S.dump_links & LINK_UPDATE) { js(&j, "{"); js_kstr(&j, "t", "Pick the v272 update folder"); js(&j, ","); js_kstr(&j, "a", "browse_update"); js(&j, "}"); first = false; }
    if (S.dump_links & LINK_KEYS) { js(&j, first ? "{" : ",{"); js_kstr(&j, "t", "Pick keys.txt"); js(&j, ","); js_kstr(&j, "a", "browse_keys"); js(&j, "}"); }
    js(&j, "]}");
    // install location
    js(&j, ",\"inst\":{"); js_kstr(&j, "path", S.inst); js(&j, ",");
    snprintf(t, sizeof(t), "Needs %.1f GB", S.inst_need_gb); js_kstr(&j, "need", S.inst_msg[0] ? S.inst_msg : t); js(&j, ",");
    if (S.inst[0] && S.inst_free_gb > 0) snprintf(t, sizeof(t), "%.0f GB free on %.2s", S.inst_free_gb, S.inst); else t[0] = 0;
    js_kstr(&j, "free", t); js(&j, ","); js_kbool(&j, "ok", S.inst_ok); js(&j, "}");
    // progress card
    char lbl[400] = "", left[160] = ""; double pct = 0; bool warn = false;
    if (S.building) {
        if (S.cancelling) snprintf(lbl, sizeof(lbl), "Stopping" ELL);
        else snprintf(lbl, sizeof(lbl), "Step %d of 3 " DOT " %s%s%s" ELL, S.bstep, S.bstep_text, S.detail[0] ? " " DOT " " : "", S.detail);
        pct = S.progress * 100;
        double el = (GetTickCount64() - S.build_t0) / 1000.0;
        if (S.dump_is_image && S.bstep == 1 && S.progress > 0.05) { double rem = el * (1 - S.progress) / S.progress; snprintf(left, sizeof(left), "about %d min left", (int)(rem / 60) + 1); }
        else snprintf(left, sizeof(left), "%.0f s", el);
    } else if (S.built) {
        snprintf(lbl, sizeof(lbl), "Installed " DOT " ready to play");
        pct = 100;
        if (S.size_gb <= 0) S.size_gb = dir_size(S.inst, 0) / (1024.0 * 1024 * 1024);
        snprintf(left, sizeof(left), "%.1f GB in %s", S.size_gb, S.inst);
    } else if (S.failed) {
        snprintf(lbl, sizeof(lbl), "Stopped: %s", S.fail_msg); warn = true;
        snprintf(left, sizeof(left), "details in logs\\install.txt");
    } else if (S.dump_ok && S.inst_ok) {
        snprintf(lbl, sizeof(lbl), "Ready to install");
        snprintf(left, sizeof(left), S.dump_is_image ? "a few minutes (reads the disc image)" : "takes a few seconds");
    } else snprintf(lbl, sizeof(lbl), "Pick your game and an install folder");
    js(&j, ",\"prog\":{"); js_kstr(&j, "lbl", lbl); js(&j, ","); js_knum(&j, "pct", pct); js(&j, ","); js_kstr(&j, "left", left); js(&j, ","); js_kbool(&j, "warn", warn); js(&j, "}");
    // the big button
    const char* mt = S.building ? (S.cancelling ? "Stopping" ELL : "Cancel") : S.built ? "Play" : "Install";
    bool men = S.building ? !S.cancelling : S.built || (S.dump_ok && S.inst_ok);
    js(&j, ",\"main\":{"); js_kstr(&j, "t", mt); js(&j, ","); js_kbool(&j, "en", men); js(&j, "}");
    js(&j, ","); js_kbool(&j, "busy", S.building);
    js(&j, ","); js_kbool(&j, "music", S.music_on);
    js(&j, ","); js_kbool(&j, "reduce", S.reduce_motion);
    js(&j, ","); js_kbool(&j, "measure", S.building);
    if (!S.vol_sent) { S.vol_sent = true; js(&j, ","); js_knum(&j, "vol", S.volume); }
    // mods
    js(&j, ",\"mods\":{");
    snprintf(t, sizeof(t), "%s\\mods", S.inst);
    js_kstr(&j, "dir", is_nsr_install(S.inst) ? t : ""); js(&j, ","); js_kstr(&j, "msg", g_mods_msg); js(&j, ","); js_kbool(&j, "warn", g_mods_warn); js(&j, ",");
    js_kstr(&j, "empty", g_mods_empty); js(&j, ",\"list\":[");
    for (int i = 0; i < g_nmods; i++) {
        js(&j, i ? ",{" : "{");
        js_kstr(&j, "n", g_mods[i].name); js(&j, ","); js_kstr(&j, "d", g_mods[i].desc); js(&j, ","); js_kstr(&j, "s", g_mods[i].status); js(&j, ",");
        js_kbool(&j, "on", g_mods[i].on); js(&j, ","); js_kbool(&j, "ok", g_mods[i].ok); js(&j, "}");
    }
    js(&j, "]}");
    // the settings screen (the page's own; the "settings" popup below stays for older pages)
    js(&j, ",\"opts\":{"); js_kbool(&j, "fullscreen", S.fullscreen); js(&j, ","); js_kbool(&j, "fps", S.show_fps); js(&j, ",");
    js_kbool(&j, "perf", S.perf_mode); js(&j, ","); js_kbool(&j, "music", S.music_on); js(&j, ","); js_kbool(&j, "reduce", S.reduce_motion); js(&j, ",");
    js_kbool(&j, "canPlay", S.built && S.dump_ok && S.inst_ok); js(&j, ","); js_kbool(&j, "hasDir", is_dir(S.inst)); js(&j, ",");
    js_kbool(&j, "isInst", is_nsr_install(S.inst)); js(&j, "}}");
    push_json(j.b);
    free(j.b);
}

// ---- playing: the window closes, the game runs in this process -----------------------------------
// (or in the install folder's copy of NSR.exe: see launch_installed)
static void webview_close(void) {
    if (S.wv) { ICoreWebView2_Release(S.wv); S.wv = NULL; }
    if (S.ctrl) { ICoreWebView2Controller_Close(S.ctrl); ICoreWebView2Controller_Release(S.ctrl); S.ctrl = NULL; }
    if (S.env) { ICoreWebView2Environment_Release(S.env); S.env = NULL; }
    S.ui_ready = false;
}
// The game's paths and switches from the install folder (the launcher's Play, and "NSR.exe --play")
static bool prepare_play(void) {
    char rpx[MAX_PATH + 48], ini[MAX_PATH + 32], mods[8192];
    snprintf(rpx, sizeof(rpx), "%s\\data\\Gambit_v272_patched.rpx", S.inst);
    if (!exists(rpx)) return false;
    snprintf(ini, sizeof(ini), "%s\\data\\install.ini", S.inst);
    GetPrivateProfileStringA("install", "game", S.dump, S.out_game, MAX_PATH, ini);   // (an image install: <install>\gamedata)
    snprintf(S.out_rpx, sizeof(S.out_rpx), "%s", rpx);
    snprintf(S.out_saves, sizeof(S.out_saves), "%s\\saves", S.inst);
    // the game's switches (the runtime reads them with getenv)
    _putenv_s("SPLAT_FULLSCREEN", S.fullscreen ? "1" : "0");
    _putenv_s("SPLAT_HIDE_FPS", S.show_fps ? "0" : "1");
    _putenv_s("SPLAT_PERF_MODE", S.perf_mode ? "1" : "0");   // (small PCs: 1x, no anti-aliasing, the plaza at the game's own 30 fps)
    if (S.bench) _putenv_s("SPLAT_BENCH", "1");   // (the benchmark: this start only; a copy started with --play inherits it)
    mods_env(mods, sizeof(mods));
    _putenv_s("SPLAT_MOD_DIRS", mods);   // ("" removes it)
    return true;
}
// The game always runs from the install folder's NSR.exe. The graphics
// driver keeps the pipelines it compiled per program path (AMD: one cache
// file per program in %LOCALAPPDATA%\\AMD\\VkCache; our own pipeline_cache.bin
// didn't stand in for it): a first trip into the plaza froze ~15-18 s for
// every new path -- played once from the extracted zip and then from the
// install folder, a player would have sat through all of it twice. So Play
// in any other copy brings the install folder's copy up to date if needed
// and starts it with --play.
static bool same_file(const char* a, const char* b) {
    WIN32_FILE_ATTRIBUTE_DATA x, y;
    if (!GetFileAttributesExA(a, GetFileExInfoStandard, &x) || !GetFileAttributesExA(b, GetFileExInfoStandard, &y)) return false;
    return x.nFileSizeHigh == y.nFileSizeHigh && x.nFileSizeLow == y.nFileSizeLow && CompareFileTime(&x.ftLastWriteTime, &y.ftLastWriteTime) == 0;
}
static bool launch_installed(void) {
    char me[MAX_PATH], exe[MAX_PATH + 16], cmd[MAX_PATH + 40], ui[MAX_PATH + 8];
    GetModuleFileNameA(NULL, me, MAX_PATH);
    snprintf(exe, sizeof(exe), "%s\\NSR.exe", S.inst);
    if (!_stricmp(me, exe)) return false;   // (this is it: play here)
    if (!same_file(me, exe)) {   // (missing or another version: this one goes there, with its page)
        lg("updating the install folder's NSR.exe from %s", me);
        if (!CopyFileA(me, exe, FALSE)) { lg("couldn't update %s (error %lu): playing from this copy", exe, GetLastError()); return false; }
        snprintf(ui, sizeof(ui), "%s\\ui", S.inst);
        if (S.ui_dir[0] && _stricmp(S.ui_dir, ui) != 0) copy_tree(S.ui_dir, ui);
    }
    snprintf(cmd, sizeof(cmd), "\"%s\" --play", exe);
    STARTUPINFOA si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, S.inst, &si, &pi)) { lg("couldn't start %s (error %lu): playing from this copy", exe, GetLastError()); return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    lg("started %s --play", exe);
    return true;
}
static void play_game(void) {
    settings_save();   // (the install folder's settings.ini: what --play reads)
    if (!prepare_play()) { MessageBoxA(S.hwnd, "The game isn't installed here yet.", "NSR", MB_OK | MB_ICONWARNING); return; }
    bool other = launch_installed();
    S.play_now = !other;   // (started the install folder's copy: this one just closes)
    ShowWindow(S.hwnd, SW_HIDE);   // (gone at once; the music stops with the page)
    webview_close();
    DestroyWindow(S.hwnd);
}
// "NSR.exe --play" (started by another copy's Play): no window, straight into the game
int nsr_play_direct(char* rpx_out, char* game_out, char* saves_out, char* inst_out) {
    GetModuleFileNameA(NULL, S.exe_dir, MAX_PATH);
    { char* s = strrchr(S.exe_dir, '\\'); if (s) *s = 0; }
    settings_load();
    snprintf(S.inst, MAX_PATH, "%s", S.exe_dir);   // (this copy is the install folder's)
    if (!prepare_play()) { MessageBoxA(NULL, "The game isn't installed in this folder.", "NSR", MB_OK | MB_ICONWARNING); return 0; }
    snprintf(rpx_out, MAX_PATH, "%s", S.out_rpx);
    snprintf(game_out, MAX_PATH, "%s", S.out_game);
    snprintf(saves_out, MAX_PATH, "%s", S.out_saves);
    snprintf(inst_out, MAX_PATH, "%s", S.inst);
    return 1;
}

// ---- commands from the page -------------------------------------------------------------------------
static void refresh_all(void) { check_dump(); check_install_dir(); check_built(); mods_scan(); }
static void on_command(const char* c) {
    if (strncmp(c, "fps:", 4)) lg("page: %s", c);
    if (!strcmp(c, "ready")) {
        S.ui_ready = true; S.vol_sent = false;
        if (S.ctrl && !IsIconic(S.hwnd)) { ICoreWebView2Controller_put_IsVisible(S.ctrl, TRUE); layout(); }
        lg("page shown");
        push_state();
        if (S.auto_test) SetTimer(S.hwnd, 4, 3000, NULL);   // (tests: install / Play 3 s after the page is up)
        return;
    }
    if (!strncmp(c, "fps:", 4)) {   // the page's frames per second and longest frame, every second while installing
        int f = 0, g = 0;
        if (sscanf(c + 4, "%d:%d", &f, &g) == 2 && S.building) { S.fps_n++; S.fps_sum += f; if (f < S.fps_min) S.fps_min = f; if (g > S.gap_max) S.gap_max = g; }
        return;
    }
    if (!strncmp(c, "vol:", 4)) { S.volume = atoi(c + 4); settings_save(); return; }
    if (!strncmp(c, "music:", 6)) { S.music_on = c[6] == '1'; settings_save(); return; }
    if (!strncmp(c, "tab:", 4)) { S.tab = !strcmp(c + 4, "mods"); if (S.tab) { mods_scan(); push_state(); } return; }
    if (!strncmp(c, "mod:", 4)) { int i = 0, on = 0; if (sscanf(c + 4, "%d:%d", &i, &on) == 2) mods_toggle(i, on); push_state(); return; }
    if (!strcmp(c, "exit")) { if (!S.building) PostMessageA(S.hwnd, WM_CLOSE, 0, 0); return; }   // (the wizard page's Exit button)
    if (S.building && strcmp(c, "main") && strcmp(c, "logs")) return;   // (nothing else while installing)
    if (!strcmp(c, "browse_dump")) {
        const WCHAR* items[5] = { L"Dump folder (Dumpling)\u2026", L"Disc image (.wux / .wud)\u2026", NULL, L"v272 update folder (for a disc image)\u2026", L"keys.txt (for a disc image)\u2026" };
        bool en[5] = { true, true, true, S.dump_is_image, S.dump_is_image };
        int r = popup(items, NULL, en, 5);
        char p[MAX_PATH] = "";
        if (r == 1 && pick_folder(L"Where is your Splatoon dump? (the folder with the game and its v272 update)", p)) snprintf(S.dump, MAX_PATH, "%s", p);
        else if (r == 2 && pick_path(L"Your Splatoon disc image", p, false, L"Wii U disc image (*.wux, *.wud)", L"*.wux;*.wud")) snprintf(S.dump, MAX_PATH, "%s", p);
        else if (r == 4 && pick_folder(L"The folder of your Splatoon v272 update dump", p)) snprintf(S.update_dir, MAX_PATH, "%s", p);
        else if (r == 5 && pick_path(L"Your keys.txt", p, false, L"keys.txt", L"*.txt")) snprintf(S.keys_file, MAX_PATH, "%s", p);
        else { push_state(); return; }
        S.failed = false;
        refresh_all(); settings_save(); push_state();
        return;
    }
    if (!strcmp(c, "browse_update")) { char p[MAX_PATH] = ""; if (pick_folder(L"The folder of your Splatoon v272 update dump", p)) { snprintf(S.update_dir, MAX_PATH, "%s", p); refresh_all(); settings_save(); } push_state(); return; }
    if (!strcmp(c, "browse_keys")) { char p[MAX_PATH] = ""; if (pick_path(L"Your keys.txt", p, false, L"keys.txt", L"*.txt")) { snprintf(S.keys_file, MAX_PATH, "%s", p); refresh_all(); settings_save(); } push_state(); return; }
    if (!strcmp(c, "browse_inst")) {
        char p[MAX_PATH] = "";
        if (pick_folder(L"Where should NSR install the game?", p)) { snprintf(S.inst, MAX_PATH, "%s", p); S.failed = false; refresh_all(); settings_save(); }
        push_state();
        return;
    }
    if (!strcmp(c, "main")) {
        if (S.building) build_cancel();
        else if (S.built) PostMessageA(S.hwnd, WM_DO_PLAY, 0, 0);
        else build_start();
        return;
    }
    if (!strcmp(c, "logs")) {
        char p[MAX_PATH + 8];
        if (S.inst[0] && is_dir(S.inst)) { snprintf(p, sizeof(p), "%s\\logs", S.inst); CreateDirectoryA(p, NULL); }
        else snprintf(p, sizeof(p), "%s", S.exe_dir);
        ShellExecuteA(NULL, "open", p, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    if (!strncmp(c, "opt:", 4)) {   // the page's settings screen: the same switches and actions as the popup below
        const char* o = c + 4;
        bool can_play = S.built && S.dump_ok && S.inst_ok;
        if (!strcmp(o, "fullscreen")) S.fullscreen = !S.fullscreen;
        else if (!strcmp(o, "fps")) S.show_fps = !S.show_fps;
        else if (!strcmp(o, "perf")) S.perf_mode = !S.perf_mode;
        else if (!strcmp(o, "music")) { S.music_on = !S.music_on; push_json(S.music_on ? "{\"playMusic\":true}" : "{\"stopMusic\":true}"); }
        else if (!strcmp(o, "reduce")) S.reduce_motion = !S.reduce_motion;
        else if (!strcmp(o, "bench")) { if (can_play) { S.bench = true; if (!S.play_now) play_game(); } return; }
        else if (!strcmp(o, "open_inst")) { if (is_dir(S.inst)) ShellExecuteA(NULL, "open", S.inst, NULL, NULL, SW_SHOWNORMAL); return; }
        else if (!strcmp(o, "repair")) { if (can_play) { S.built = false; build_start(); } }
        else if (!strcmp(o, "remove")) { if (is_nsr_install(S.inst)) remove_install(); }
        else return;
        settings_save();
        push_state();
        return;
    }
    if (!strcmp(c, "settings")) {
        bool can_play = S.built && S.dump_ok && S.inst_ok;
        const WCHAR* items[11] = { L"Fullscreen", L"FPS counter", L"Performance mode (small PCs)", L"Launcher music", L"Reduce motion", NULL,
                                   L"Run the benchmark (every map, about 30 minutes)", L"Open the install folder", L"Reinstall (repair)", NULL, L"Remove installation\u2026" };
        bool ck[11] = { S.fullscreen, S.show_fps, S.perf_mode, S.music_on, S.reduce_motion, false, false, false, false, false, false };
        bool en[11] = { true, true, true, true, true, true, can_play, is_dir(S.inst), can_play, true, is_nsr_install(S.inst) };
        int r = popup(items, ck, en, 11);
        if (r == 1) S.fullscreen = !S.fullscreen;
        else if (r == 2) S.show_fps = !S.show_fps;
        else if (r == 3) S.perf_mode = !S.perf_mode;
        else if (r == 4) { S.music_on = !S.music_on; push_json(S.music_on ? "{\"playMusic\":true}" : "{\"stopMusic\":true}"); }
        else if (r == 5) S.reduce_motion = !S.reduce_motion;
        else if (r == 7) { S.bench = true; if (!S.building && !S.play_now) play_game(); return; }   // (the game runs the benchmark by itself; results in <install>\benchmarks)
        else if (r == 8) ShellExecuteA(NULL, "open", S.inst, NULL, NULL, SW_SHOWNORMAL);
        else if (r == 9) { S.built = false; build_start(); }
        else if (r == 11) remove_install();
        if (r) settings_save();
        push_state();
        return;
    }
    if (!strcmp(c, "mods_open")) {
        if (!is_nsr_install(S.inst)) return;
        char p[MAX_PATH + 8]; snprintf(p, sizeof(p), "%s\\mods", S.inst); CreateDirectoryA(p, NULL);
        ShellExecuteA(NULL, "open", p, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    if (!strcmp(c, "mods_add")) { mods_add(); push_state(); return; }
}

// ---- WebView2 (COM objects written out in C) -------------------------------------------------------
static const IID NSR_IID_EnvDone  = { 0x4e8a3389, 0xc9d8, 0x4bd2, { 0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d } };
static const IID NSR_IID_CtrlDone = { 0x6c4819f3, 0xc9b7, 0x4260, { 0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c } };
static const IID NSR_IID_Msg      = { 0x57213f19, 0x00e6, 0x49fa, { 0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2 } };
static const IID NSR_IID_Failed   = { 0x79e0aea4, 0x990b, 0x42d9, { 0xaa, 0x1d, 0x0f, 0xcc, 0x2e, 0x5b, 0xc7, 0xf1 } };
static const IID NSR_IID_WV3      = { 0xa0d6df20, 0x3b92, 0x416d, { 0xaa, 0x0c, 0x43, 0x7a, 0x9c, 0x72, 0x78, 0x57 } };
static const IID NSR_IID_Ctrl2    = { 0xc979903e, 0xd4ca, 0x4228, { 0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab } };
static const IID NSR_IID_Set3     = { 0xfdb5ab74, 0xaf33, 0x4854, { 0x84, 0xf0, 0x0a, 0x63, 0x1d, 0xeb, 0x5e, 0xba } };
static const IID NSR_IID_Set8     = { 0x9e6b0e8f, 0x86ad, 0x4e81, { 0x81, 0x47, 0xa9, 0xb5, 0xed, 0xb6, 0x86, 0x50 } };
// (the handlers live as long as the program: AddRef / Release do nothing)
#define NSR_HANDLER_BASICS(T, IIDV) \
    static HRESULT STDMETHODCALLTYPE T##_qi(T* This, REFIID riid, void** out) { \
        if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IIDV)) { *out = This; return S_OK; } \
        *out = NULL; return E_NOINTERFACE; } \
    static ULONG STDMETHODCALLTYPE T##_addref(T* This) { (void)This; return 2; } \
    static ULONG STDMETHODCALLTYPE T##_release(T* This) { (void)This; return 1; }
NSR_HANDLER_BASICS(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, NSR_IID_EnvDone)
NSR_HANDLER_BASICS(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, NSR_IID_CtrlDone)
NSR_HANDLER_BASICS(ICoreWebView2WebMessageReceivedEventHandler, NSR_IID_Msg)
NSR_HANDLER_BASICS(ICoreWebView2ProcessFailedEventHandler, NSR_IID_Failed)

static void layout(void) {
    if (!S.ctrl) return;
    RECT r; GetClientRect(S.hwnd, &r);
    ICoreWebView2Controller_put_Bounds(S.ctrl, r);
    // the design is laid out for at least 1000 x 800 page pixels: smaller windows zoom out instead of cutting it off
    double sc = GetDpiForWindow(S.hwnd) / 96.0;
    double cw = (r.right - r.left) / sc, ch = (r.bottom - r.top) / sc, z = 1.0;
    if (cw / 1000.0 < z) z = cw / 1000.0;
    if (ch / 800.0 < z) z = ch / 800.0;
    if (z < 0.4) z = 0.4;
    ICoreWebView2Controller_put_ZoomFactor(S.ctrl, z);
}
static HRESULT STDMETHODCALLTYPE msg_invoke(ICoreWebView2WebMessageReceivedEventHandler* This, ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) {
    (void)This; (void)sender;
    LPWSTR w = NULL;
    if (SUCCEEDED(ICoreWebView2WebMessageReceivedEventArgs_TryGetWebMessageAsString(args, &w)) && w) {
        char c[1024];
        WideCharToMultiByte(CP_ACP, 0, w, -1, c, sizeof(c), NULL, NULL);
        CoTaskMemFree(w);
        // fps reports are handled right here; everything else after this callback returns (dialogs, menus)
        if (!strncmp(c, "fps:", 4)) on_command(c);
        else PostMessageA(S.hwnd, WM_UI_CMD, 0, (LPARAM)_strdup(c));
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE failed_invoke(ICoreWebView2ProcessFailedEventHandler* This, ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) {
    (void)This; (void)args;
    lg("the page's process failed");
    if (sender && !S.play_now) { S.ui_ready = false; ICoreWebView2_Reload(sender); }   // (the page's process died: load it again)
    return S_OK;
}
static ICoreWebView2WebMessageReceivedEventHandlerVtbl msg_vt = { ICoreWebView2WebMessageReceivedEventHandler_qi, ICoreWebView2WebMessageReceivedEventHandler_addref, ICoreWebView2WebMessageReceivedEventHandler_release, msg_invoke };
static ICoreWebView2WebMessageReceivedEventHandler msg_handler = { &msg_vt };
static ICoreWebView2ProcessFailedEventHandlerVtbl failed_vt = { ICoreWebView2ProcessFailedEventHandler_qi, ICoreWebView2ProcessFailedEventHandler_addref, ICoreWebView2ProcessFailedEventHandler_release, failed_invoke };
static ICoreWebView2ProcessFailedEventHandler failed_handler = { &failed_vt };

static void ui_error(const char* what, HRESULT hr) {
    char m[600];
    snprintf(m, sizeof(m), "NSR's window couldn't start (%s, error 0x%08lX).\n\nIt needs the Microsoft Edge WebView2 Runtime, which comes with Windows 11 and current Windows 10. "
             "If it's missing, install \"Microsoft Edge WebView2 Runtime\" from Microsoft and start NSR again.", what, (unsigned long)hr);
    MessageBoxA(S.hwnd, m, "NSR", MB_OK | MB_ICONERROR);
    DestroyWindow(S.hwnd);
}
static HRESULT STDMETHODCALLTYPE ctrl_invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* This, HRESULT hr, ICoreWebView2Controller* ctrl) {
    (void)This;
    lg("controller created: 0x%08lX", (unsigned long)hr);
    if (FAILED(hr) || !ctrl) { ui_error("creating the page view", hr); return S_OK; }
    S.ctrl = ctrl; ICoreWebView2Controller_AddRef(ctrl);
    ICoreWebView2Controller_get_CoreWebView2(ctrl, &S.wv);
    {   // the window's background while the page loads: the design's darkest blue (no white flash)
        ICoreWebView2Controller2* c2 = NULL;
        if (SUCCEEDED(ICoreWebView2Controller_QueryInterface(ctrl, &NSR_IID_Ctrl2, (void**)&c2)) && c2) {
            COREWEBVIEW2_COLOR col = { 255, 6, 20, 51 };
            ICoreWebView2Controller2_put_DefaultBackgroundColor(c2, col);
            ICoreWebView2Controller2_Release(c2);
        }
    }
    ICoreWebView2Settings* st = NULL;
    if (SUCCEEDED(ICoreWebView2_get_Settings(S.wv, &st)) && st) {
        BOOL dev = getenv("NSR_DEVTOOLS") != NULL;
        ICoreWebView2Settings_put_AreDevToolsEnabled(st, dev);
        ICoreWebView2Settings_put_AreDefaultContextMenusEnabled(st, dev);
        ICoreWebView2Settings_put_IsStatusBarEnabled(st, FALSE);
        ICoreWebView2Settings_put_IsZoomControlEnabled(st, FALSE);
        ICoreWebView2Settings3* s3 = NULL;
        if (SUCCEEDED(ICoreWebView2Settings_QueryInterface(st, &NSR_IID_Set3, (void**)&s3)) && s3) {
            ICoreWebView2Settings3_put_AreBrowserAcceleratorKeysEnabled(s3, dev);   // (no F5 / Ctrl+R / Ctrl+F in the launcher)
            ICoreWebView2Settings3_Release(s3);
        }
        ICoreWebView2Settings8* s8 = NULL;   // (no SmartScreen lookups for the launcher's own local page)
        if (SUCCEEDED(ICoreWebView2Settings_QueryInterface(st, &NSR_IID_Set8, (void**)&s8)) && s8) {
            ICoreWebView2Settings8_put_IsReputationCheckingRequired(s8, FALSE);
            ICoreWebView2Settings8_Release(s8);
        }
        ICoreWebView2Settings_Release(st);
    }
    EventRegistrationToken tok;
    ICoreWebView2Controller_put_IsVisible(ctrl, FALSE);   // (the splash stays until the page says it's ready)
    ICoreWebView2_add_WebMessageReceived(S.wv, &msg_handler, &tok);
    ICoreWebView2_add_ProcessFailed(S.wv, &failed_handler, &tok);
    layout();
    // ui\ is served as https://nsr.ui/ (so the page's assets/... paths work)
    ICoreWebView2_3* w3 = NULL;
    if (FAILED(ICoreWebView2_QueryInterface(S.wv, &NSR_IID_WV3, (void**)&w3)) || !w3) { ui_error("this WebView2 Runtime is too old", E_NOINTERFACE); return S_OK; }
    WCHAR wd[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, S.ui_dir, -1, wd, MAX_PATH);
    ICoreWebView2_3_SetVirtualHostNameToFolderMapping(w3, L"nsr.ui", wd, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    ICoreWebView2_3_Release(w3);
    hr = ICoreWebView2_Navigate(S.wv, L"https://nsr.ui/nsr_loader_ui.html");
    lg("navigate to the page in %s: 0x%08lX", S.ui_dir, (unsigned long)hr);
    return S_OK;
}
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl ctrl_vt = { ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_qi, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_addref, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_release, ctrl_invoke };
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandler ctrl_handler = { &ctrl_vt };
static HRESULT STDMETHODCALLTYPE env_invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* This, HRESULT hr, ICoreWebView2Environment* env) {
    (void)This;
    lg("environment created: 0x%08lX", (unsigned long)hr);
    if (FAILED(hr) || !env) { ui_error("starting WebView2", hr); return S_OK; }
    S.env = env; ICoreWebView2Environment_AddRef(env);
    hr = ICoreWebView2Environment_CreateCoreWebView2Controller(env, S.hwnd, &ctrl_handler);
    if (FAILED(hr)) ui_error("creating the page view", hr);
    return S_OK;
}
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl env_vt = { ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_qi, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_addref, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_release, env_invoke };
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler env_handler = { &env_vt };

// ---- the splash: drawn by the window itself until the page is ready (WebView2 needs a few seconds to start) ----
static void splash_paint(HWND h, HDC dc) {
    RECT r; GetClientRect(h, &r);
    int W = r.right, H = r.bottom;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W > 0 ? W : 1, H > 0 ? H : 1);
    HGDIOBJ old = SelectObject(mem, bmp);
    // the design's background: lighter blue at the top centre fading to deep navy
    for (int y = 0; y < H; y += 4) {
        double f = H > 1 ? (double)y / H : 0;
        int cr = (int)(29 + (6 - 29) * f), cg = (int)(79 + (20 - 79) * f), cb = (int)(174 + (51 - 174) * f);
        RECT band = { 0, y, W, y + 4 };
        HBRUSH b = CreateSolidBrush(RGB(cr, cg, cb)); FillRect(mem, &band, b); DeleteObject(b);
    }
    UINT dpi = GetDpiForWindow(h);
    SetBkMode(mem, TRANSPARENT);
    HFONT big = CreateFontW(-MulDiv(44, dpi, 96), 0, 0, 0, FW_HEAVY, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT mid = CreateFontW(-MulDiv(16, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT sm = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    int cy = H / 2;
    RECT t1 = { 0, cy - MulDiv(70, dpi, 96), W, cy - MulDiv(10, dpi, 96) };
    SelectObject(mem, big); SetTextColor(mem, RGB(255, 255, 255));
    DrawTextW(mem, L"NSR", -1, &t1, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
    RECT t2 = { 0, cy - MulDiv(6, dpi, 96), W, cy + MulDiv(22, dpi, 96) };
    SelectObject(mem, mid); SetTextColor(mem, RGB(159, 200, 245));
    DrawTextW(mem, L"Native Splat Recomp Loader", -1, &t2, DT_CENTER | DT_TOP | DT_SINGLELINE);
    static const WCHAR* dots[4] = { L"Starting", L"Starting.", L"Starting..", L"Starting..." };
    RECT t3 = { 0, cy + MulDiv(40, dpi, 96), W, cy + MulDiv(64, dpi, 96) };
    SelectObject(mem, sm); SetTextColor(mem, RGB(127, 208, 255));
    DrawTextW(mem, dots[(S.splash_tick / 4) % 4], -1, &t3, DT_CENTER | DT_TOP | DT_SINGLELINE);
    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    DeleteObject(big); DeleteObject(mid); DeleteObject(sm);
}

// ---- the window ----------------------------------------------------------------------------------
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        if (!S.ui_ready) splash_paint(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (!S.ui_ready) InvalidateRect(h, NULL, FALSE);
        if (S.ctrl) {
            BOOL vis = w != SIZE_MINIMIZED && S.ui_ready;   // (minimized: the page is hidden, its animations and music timer pause)
            ICoreWebView2Controller_put_IsVisible(S.ctrl, vis);
            if (vis) layout();
        }
        return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_MOVE: case WM_MOVING: if (S.ctrl) ICoreWebView2Controller_NotifyParentWindowPositionChanged(S.ctrl); break;
    case WM_GETMINMAXINFO: { MINMAXINFO* mi = (MINMAXINFO*)l; UINT dpi = GetDpiForWindow(h); mi->ptMinTrackSize.x = MulDiv(640, dpi, 96); mi->ptMinTrackSize.y = MulDiv(520, dpi, 96); return 0; }
    case WM_TIMER:
        if (w == 1 && S.dirty) push_state();
        if (w == 1 && !S.ui_ready) { S.splash_tick++; if (S.splash_tick % 4 == 0) InvalidateRect(h, NULL, FALSE); }
        if (w == 2 && S.building) S.dirty = true;   // (the elapsed time / time left)
        if (w == 3) { KillTimer(h, 3); PostMessageA(h, WM_DO_PLAY, 0, 0); }   // (NSR_TEST_AUTO: Play after the install)
        if (w == 4) { KillTimer(h, 4); if (S.built) PostMessageA(h, WM_DO_PLAY, 0, 0); else build_start(); }
        return 0;
    case WM_UI_CMD: { char* c = (char*)l; on_command(c); free(c); return 0; }
    case WM_DO_PLAY: if (!S.building && !S.play_now) play_game(); return 0;
    case WM_BUILD_LINE: { char* s = (char*)l; build_line(s); free(s); return 0; }
    case WM_BUILD_EXIT:
        build_exit((DWORD)w);
        if (S.auto_test && S.built) SetTimer(h, 3, 3000, NULL);
        return 0;
    case WM_CLOSE:
        if (S.building) {
            if (MessageBoxA(h, "NSR is still installing. Stop and close?", "NSR", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return 0;
            nsr_wud_cancel = 1;
            if (S.worker) WaitForSingleObject(S.worker, 15000);
        }
        webview_close();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
// launcher\ui (the page) next to NSR.exe, in the install folder, or in the project when run from a build folder
static bool find_ui_dir(void) {
    char d[MAX_PATH], p[MAX_PATH + 64];
    snprintf(d, sizeof(d), "%s", S.exe_dir);
    for (int up = 0; up < 5; up++) {
        snprintf(p, sizeof(p), "%s\\ui\\nsr_loader_ui.html", d);
        if (exists(p)) { snprintf(S.ui_dir, MAX_PATH, "%s\\ui", d); return true; }
        snprintf(p, sizeof(p), "%s\\launcher\\ui\\nsr_loader_ui.html", d);
        if (exists(p)) { snprintf(S.ui_dir, MAX_PATH, "%s\\launcher\\ui", d); return true; }
        char* s = strrchr(d, '\\');
        if (!s) break;
        *s = 0;
    }
    return false;
}

// The launcher window. Returns 1 with the game's paths when Play was pressed, 0 when the window was closed.
int nsr_wizard(char* rpx_out, char* game_out, char* saves_out, char* inst_out) {
    HINSTANCE inst = GetModuleHandleA(NULL);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetModuleFileNameA(NULL, S.exe_dir, MAX_PATH);
    { char* s = strrchr(S.exe_dir, '\\'); if (s) *s = 0; }
    S.auto_test = getenv("NSR_TEST_AUTO") != NULL;
    settings_load();
    if (!find_ui_dir()) {
        MessageBoxA(NULL, "NSR's ui folder is missing (ui\\nsr_loader_ui.html next to NSR.exe). Extract the whole zip, not just NSR.exe.", "NSR", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 0;
    }
    if (!S.inst[0]) default_install();
    refresh_all();

    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = "NSRLauncher";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(10, 34, 89));
    { char ip[MAX_PATH + 32]; snprintf(ip, sizeof(ip), "%s\\assets\\nsr_logo.ico", S.ui_dir); wc.hIcon = (HICON)LoadImageA(NULL, ip, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE); }
    if (!wc.hIcon) wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);
    // 1280 x 720 page pixels (the page is laid out 16:9), shrunk to the screen's work area
    UINT dpi = GetDpiForSystem();
    RECT wr = { 0, 0, MulDiv(1280, dpi, 96), MulDiv(720, dpi, 96) };
    AdjustWindowRectExForDpi(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    RECT work; SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    if (ww > work.right - work.left) ww = work.right - work.left;
    if (wh > work.bottom - work.top) wh = work.bottom - work.top;
    int wx = work.left + (work.right - work.left - ww) / 2, wy = work.top + (work.bottom - work.top - wh) / 2;
    S.hwnd = CreateWindowExA(0, "NSRLauncher", "NSR - Native Splat Recomp Loader", WS_OVERLAPPEDWINDOW, wx, wy, ww, wh, NULL, NULL, inst, NULL);
    ShowWindow(S.hwnd, SW_SHOWNORMAL);
    UpdateWindow(S.hwnd);
    SetTimer(S.hwnd, 1, 100, NULL);
    SetTimer(S.hwnd, 2, 1000, NULL);

    // WebView2: its cache next to NSR.exe; music may start without a click
    char ud[MAX_PATH + 16];
    snprintf(ud, sizeof(ud), "%s\\webview", S.exe_dir);
    { char e[MAX_PATH]; if (GetEnvironmentVariableA("NSR_WV2_DATA", e, MAX_PATH)) snprintf(ud, sizeof(ud), "%s", e); }   // (tests)
    WCHAR wud[MAX_PATH + 16];
    MultiByteToWideChar(CP_ACP, 0, ud, -1, wud, MAX_PATH + 16);
    {   // NSR_WV2_ARGS: other browser arguments (tests)
        WCHAR args[512] = L"--autoplay-policy=no-user-gesture-required --enable-features=NetworkServiceInProcess,NetworkServiceInProcess2";
        char e[400];
        if (GetEnvironmentVariableA("NSR_WV2_ARGS", e, sizeof(e))) { MultiByteToWideChar(CP_ACP, 0, e, -1, args, 512); lg("browser arguments: %s", e); }
        SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", args);
    }
    CreateDirectoryA(ud, NULL);
    lg("NSR %s started from %s (ui %s)", NSR_VERSION, S.exe_dir, S.ui_dir);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(NULL, wud, NULL, &env_handler);
    lg("CreateCoreWebView2EnvironmentWithOptions: 0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) ui_error("starting WebView2", hr);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    webview_close();
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", NULL);
    CoUninitialize();   // (the runtime's audio initializes COM its own way afterwards)
    if (!S.play_now) return 0;
    snprintf(rpx_out, MAX_PATH, "%s", S.out_rpx);
    snprintf(game_out, MAX_PATH, "%s", S.out_game);
    snprintf(saves_out, MAX_PATH, "%s", S.out_saves);
    snprintf(inst_out, MAX_PATH, "%s", S.inst);
    return 1;
}
