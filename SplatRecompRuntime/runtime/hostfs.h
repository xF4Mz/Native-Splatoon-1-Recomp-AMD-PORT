// Small portable layer over the PC's own file system (Windows or POSIX).
#ifndef HOSTFS_H
#define HOSTFS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool exists;
    bool is_dir;
    uint64_t size;
    int64_t mtime;   // seconds since 1970
} HostStat;

HostStat host_stat(const char* path);
bool host_mkdir(const char* path);        // one level; true if it exists afterwards
bool host_mkdirs(const char* path);       // all levels
bool host_remove(const char* path);       // file or empty folder

// Calls fn for every entry in a folder (not "." or ".."). Returns false if
// the folder can't be opened.
typedef void (*HostDirFn)(const char* name, bool is_dir, void* user);
bool host_list_dir(const char* path, HostDirFn fn, void* user);

#endif
