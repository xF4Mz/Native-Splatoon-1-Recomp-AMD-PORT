#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#include "hostfs.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

HostStat host_stat(const char* path) {
    HostStat r = { 0 };
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return r;
    r.is_dir = (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return r;
    r.is_dir = S_ISDIR(st.st_mode);
#endif
    r.exists = true;
    r.size = (uint64_t)st.st_size;
    r.mtime = (int64_t)st.st_mtime;
    return r;
}

bool host_mkdir(const char* path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
    return host_stat(path).is_dir;
}

bool host_mkdirs(const char* path) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char* p = buf + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        if (p[-1] == ':') continue;   // "F:/"
        char saved = *p;
        *p = 0;
        host_mkdir(buf);
        *p = saved;
    }
    return host_mkdir(buf);
}

bool host_remove(const char* path) {
    HostStat st = host_stat(path);
    if (!st.exists) return false;
#ifdef _WIN32
    return st.is_dir ? _rmdir(path) == 0 : remove(path) == 0;
#else
    return st.is_dir ? rmdir(path) == 0 : remove(path) == 0;
#endif
}

bool host_list_dir(const char* path, HostDirFn fn, void* user) {
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof(pattern), "%s/*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        fn(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
#else
    DIR* d = opendir(path);
    if (!d) return false;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1100];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        fn(e->d_name, host_stat(full).is_dir, user);
    }
    closedir(d);
    return true;
#endif
}
